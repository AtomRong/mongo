/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/shard_metadata_util.h"

#include <memory>

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/s/type_shard_collection.h"
#include "mongo/db/s/type_shard_database.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/write_ops/batched_command_response.h"

namespace mongo {
namespace shardmetadatautil {
namespace {

const WriteConcernOptions kLocalWriteConcern(1,
                                             WriteConcernOptions::SyncMode::UNSET,
                                             Milliseconds(0));

/**
 * Processes a command result for errors, including write concern errors.
 */
Status getStatusFromWriteCommandResponse(const BSONObj& commandResult) {
    BatchedCommandResponse batchResponse;
    std::string errmsg;
    if (!batchResponse.parseBSON(commandResult, &errmsg)) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "Failed to parse write response: " << errmsg);
    }

    return batchResponse.toStatus();
}

}  // namespace

QueryAndSort createShardChunkDiffQuery(const ChunkVersion& collectionVersion) {
    return {BSON(ChunkType::lastmod() << BSON("$gte" << Timestamp(collectionVersion.toLong()))),
            BSON(ChunkType::lastmod() << 1)};
}

bool RefreshState::operator==(const RefreshState& other) const {
    return (other.epoch == epoch) && (other.refreshing == refreshing) &&
        (other.lastRefreshedCollectionVersion == lastRefreshedCollectionVersion);
}

std::string RefreshState::toString() const {
    return str::stream() << "epoch: " << epoch
                         << ", refreshing: " << (refreshing ? "true" : "false")
                         << ", lastRefreshedCollectionVersion: "
                         << lastRefreshedCollectionVersion.toString();
}

Status unsetPersistedRefreshFlags(OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  const ChunkVersion& refreshedVersion) {
    // Set 'refreshing' to false and update the last refreshed collection version.
    BSONObjBuilder updateBuilder;
    updateBuilder.append(ShardCollectionType::kRefreshingFieldName, false);
    updateBuilder.appendTimestamp(ShardCollectionType::kLastRefreshedCollectionVersionFieldName,
                                  refreshedVersion.toLong());

    return updateShardCollectionsEntry(opCtx,
                                       BSON(ShardCollectionType::kNssFieldName << nss.ns()),
                                       BSON("$set" << updateBuilder.obj()),
                                       false /*upsert*/);
}

StatusWith<RefreshState> getPersistedRefreshFlags(OperationContext* opCtx,
                                                  const NamespaceString& nss) {
    auto statusWithCollectionEntry = readShardCollectionsEntry(opCtx, nss);
    if (!statusWithCollectionEntry.isOK()) {
        return statusWithCollectionEntry.getStatus();
    }
    ShardCollectionType entry = statusWithCollectionEntry.getValue();

    // Ensure the results have not been incorrectly set somehow.
    if (entry.getRefreshing()) {
        // If 'refreshing' is present and false, a refresh must have occurred (otherwise the field
        // would never have been added to the document) and there should always be a refresh
        // version.
        invariant(*entry.getRefreshing() ? true : !!entry.getLastRefreshedCollectionVersion());
    } else {
        // If 'refreshing' is not present, no refresh version should exist.
        invariant(!entry.getLastRefreshedCollectionVersion());
    }

    return RefreshState{entry.getEpoch(),
                        // If the refreshing field has not yet been added, this means that the first
                        // refresh has started, but no chunks have ever yet been applied, around
                        // which these flags are set. So default to refreshing true because the
                        // chunk metadata is being updated and is not yet ready to be read.
                        entry.getRefreshing() ? *entry.getRefreshing() : true,
                        entry.getLastRefreshedCollectionVersion()
                            ? *entry.getLastRefreshedCollectionVersion()
                            : ChunkVersion(0, 0, entry.getEpoch(), entry.getTimestamp())};
}

StatusWith<ShardCollectionType> readShardCollectionsEntry(OperationContext* opCtx,
                                                          const NamespaceString& nss) {

    try {
        DBDirectClient client(opCtx);
        std::unique_ptr<DBClientCursor> cursor =
            client.query(NamespaceString::kShardConfigCollectionsNamespace,
                         BSON(ShardCollectionType::kNssFieldName << nss.ns()),
                         Query(),
                         1);
        if (!cursor) {
            return Status(ErrorCodes::OperationFailed,
                          str::stream() << "Failed to establish a cursor for reading "
                                        << NamespaceString::kShardConfigCollectionsNamespace.ns()
                                        << " from local storage");
        }

        if (!cursor->more()) {
            // The collection has been dropped.
            return Status(ErrorCodes::NamespaceNotFound,
                          str::stream() << "collection " << nss.ns() << " not found");
        }

        BSONObj document = cursor->nextSafe();
        return ShardCollectionType(document);
    } catch (const DBException& ex) {
        return ex.toStatus(str::stream() << "Failed to read the '" << nss.ns()
                                         << "' entry locally from config.collections");
    }
}

StatusWith<ShardDatabaseType> readShardDatabasesEntry(OperationContext* opCtx, StringData dbName) {
    try {
        DBDirectClient client(opCtx);
        std::unique_ptr<DBClientCursor> cursor =
            client.query(NamespaceString::kShardConfigDatabasesNamespace,
                         BSON(ShardDatabaseType::name() << dbName.toString()),
                         Query(),
                         1);
        if (!cursor) {
            return Status(ErrorCodes::OperationFailed,
                          str::stream() << "Failed to establish a cursor for reading "
                                        << NamespaceString::kShardConfigDatabasesNamespace.ns()
                                        << " from local storage");
        }

        if (!cursor->more()) {
            // The database has been dropped.
            return Status(ErrorCodes::NamespaceNotFound,
                          str::stream() << "database " << dbName.toString() << " not found");
        }

        BSONObj document = cursor->nextSafe();
        auto statusWithDatabaseEntry = ShardDatabaseType::fromBSON(document);
        if (!statusWithDatabaseEntry.isOK()) {
            return statusWithDatabaseEntry.getStatus();
        }

        return statusWithDatabaseEntry.getValue();
    } catch (const DBException& ex) {
        return ex.toStatus(str::stream() << "Failed to read the '" << dbName.toString()
                                         << "' entry locally from config.databases");
    }
}

Status updateShardCollectionsEntry(OperationContext* opCtx,
                                   const BSONObj& query,
                                   const BSONObj& update,
                                   const bool upsert) {
    invariant(query.hasField("_id"));
    if (upsert) {
        // If upserting, this should be an update from the config server that does not have shard
        // refresh / migration inc signal information.
        invariant(!update.hasField(ShardCollectionType::kLastRefreshedCollectionVersionFieldName));
    }

    try {
        DBDirectClient client(opCtx);
        auto commandResponse = client.runCommand([&] {
            write_ops::UpdateCommandRequest updateOp(
                NamespaceString::kShardConfigCollectionsNamespace);
            updateOp.setUpdates({[&] {
                write_ops::UpdateOpEntry entry;
                entry.setQ(query);
                entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(update));
                entry.setUpsert(upsert);
                return entry;
            }()});
            return updateOp.serialize({});
        }());
        uassertStatusOK(getStatusFromWriteCommandResponse(commandResponse->getCommandReply()));

        return Status::OK();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

Status updateShardDatabasesEntry(OperationContext* opCtx,
                                 const BSONObj& query,
                                 const BSONObj& update,
                                 const BSONObj& inc,
                                 const bool upsert) {
    invariant(query.hasField("_id"));
    if (upsert) {
        // If upserting, this should be an update from the config server that does not have shard
        // migration inc signal information.
        invariant(inc.isEmpty());
    }

    try {
        DBDirectClient client(opCtx);

        BSONObjBuilder builder;
        if (!update.isEmpty()) {
            // Want to modify the document if it already exists, not replace it.
            builder.append("$set", update);
        }
        if (!inc.isEmpty()) {
            builder.append("$inc", inc);
        }

        auto commandResponse = client.runCommand([&] {
            write_ops::UpdateCommandRequest updateOp(
                NamespaceString::kShardConfigDatabasesNamespace);
            updateOp.setUpdates({[&] {
                write_ops::UpdateOpEntry entry;
                entry.setQ(query);
                entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(builder.obj()));
                entry.setUpsert(upsert);
                return entry;
            }()});
            return updateOp.serialize({});
        }());
        uassertStatusOK(getStatusFromWriteCommandResponse(commandResponse->getCommandReply()));

        return Status::OK();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

StatusWith<std::vector<ChunkType>> readShardChunks(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   const UUID& uuid,
                                                   SupportingLongNameStatusEnum supportingLongName,
                                                   const BSONObj& query,
                                                   const BSONObj& sort,
                                                   boost::optional<long long> limit,
                                                   const OID& epoch,
                                                   const boost::optional<Timestamp>& timestamp) {
    const auto chunksNsPostfix{supportingLongName == SupportingLongNameStatusEnum::kDisabled ||
                                       nss.isTemporaryReshardingCollection()
                                   ? nss.ns()
                                   : uuid.toString()};
    const NamespaceString chunksNss{ChunkType::ShardNSPrefix + chunksNsPostfix};

    try {
        DBDirectClient client(opCtx);

        std::unique_ptr<DBClientCursor> cursor =
            client.query(chunksNss, query, Query().sort(sort), limit.get_value_or(0));
        uassert(ErrorCodes::OperationFailed,
                str::stream() << "Failed to establish a cursor for reading " << chunksNss.ns()
                              << " from local storage",
                cursor);

        std::vector<ChunkType> chunks;
        while (cursor->more()) {
            BSONObj document = cursor->nextSafe().getOwned();
            auto statusWithChunk = ChunkType::fromShardBSON(document, epoch, timestamp);
            if (!statusWithChunk.isOK()) {
                return statusWithChunk.getStatus().withContext(
                    str::stream() << "Failed to parse chunk '" << document.toString() << "'");
            }

            chunks.push_back(std::move(statusWithChunk.getValue()));
        }

        return chunks;
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

Status updateShardChunks(OperationContext* opCtx,
                         const NamespaceString& nss,
                         const UUID& uuid,
                         SupportingLongNameStatusEnum supportingLongName,
                         const std::vector<ChunkType>& chunks,
                         const OID& currEpoch) {
    invariant(!chunks.empty());

    const auto chunksNsPostfix{supportingLongName == SupportingLongNameStatusEnum::kDisabled ||
                                       nss.isTemporaryReshardingCollection()
                                   ? nss.ns()
                                   : uuid.toString()};
    const NamespaceString chunksNss{ChunkType::ShardNSPrefix + chunksNsPostfix};

    try {
        DBDirectClient client(opCtx);

        // This may be the first update, so the first opportunity to create an index.
        // If the index already exists, this is a no-op.
        client.createIndex(chunksNss.ns(), BSON(ChunkType::lastmod() << 1));

        /**
         * Here are examples of the operations that can happen on the config server to update
         * the config.cache.chunks collection. 'chunks' only includes the chunks that result from
         * the operations, which can be read from the config server, not any that were removed, so
         * we must delete any chunks that overlap with the new 'chunks'.
         *
         * CollectionVersion = 10.3
         *
         * moveChunk
         * {_id: 3, max: 5, version: 10.1} --> {_id: 3, max: 5, version: 11.0}
         *
         * splitChunk
         * {_id: 3, max: 9, version 10.3} --> {_id: 3, max: 5, version 10.4}
         *                                    {_id: 5, max: 8, version 10.5}
         *                                    {_id: 8, max: 9, version 10.6}
         *
         * mergeChunk
         * {_id: 10, max: 14, version 4.3} --> {_id: 10, max: 22, version 10.4}
         * {_id: 14, max: 19, version 7.1}
         * {_id: 19, max: 22, version 2.0}
         *
         */
        for (auto& chunk : chunks) {
            invariant(chunk.getVersion().epoch() == currEpoch);

            // Delete any overlapping chunk ranges. Overlapping chunks will have a min value
            // ("_id") between (chunk.min, chunk.max].
            //
            // query: { "_id" : {"$gte": chunk.min, "$lt": chunk.max}}
            auto deleteCommandResponse = client.runCommand([&] {
                write_ops::DeleteCommandRequest deleteOp(chunksNss);
                deleteOp.setDeletes({[&] {
                    write_ops::DeleteOpEntry entry;
                    entry.setQ(BSON(ChunkType::minShardID
                                    << BSON("$gte" << chunk.getMin() << "$lt" << chunk.getMax())));
                    entry.setMulti(true);
                    return entry;
                }()});
                return deleteOp.serialize({});
            }());
            uassertStatusOK(
                getStatusFromWriteCommandResponse(deleteCommandResponse->getCommandReply()));

            // Now the document can be expected to cleanly insert without overlap
            auto insertCommandResponse = client.runCommand([&] {
                write_ops::InsertCommandRequest insertOp(chunksNss);
                insertOp.setDocuments({chunk.toShardBSON()});
                return insertOp.serialize({});
            }());
            uassertStatusOK(
                getStatusFromWriteCommandResponse(insertCommandResponse->getCommandReply()));
        }

        return Status::OK();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

void updateSupportingLongNameOnShardCollections(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                SupportingLongNameStatusEnum supportingLongName) {
    write_ops::UpdateCommandRequest commandRequest(
        NamespaceString::kShardConfigCollectionsNamespace, [&] {
            BSONObj modifiers = supportingLongName != SupportingLongNameStatusEnum::kDisabled
                ? BSON("$set" << BSON(CollectionType::kSupportingLongNameFieldName
                                      << SupportingLongNameStatus_serializer(supportingLongName)))
                : BSON("$unset" << BSON(CollectionType::kSupportingLongNameFieldName << 1));

            write_ops::UpdateOpEntry updateOp;
            updateOp.setQ(BSON(ShardCollectionType::kNssFieldName << nss.ns()));
            updateOp.setU(write_ops::UpdateModification::parseFromClassicUpdate(modifiers));
            return std::vector{updateOp};
        }());

    DBDirectClient dbClient(opCtx);
    const auto commandResponse = dbClient.runCommand(commandRequest.serialize({}));
    uassertStatusOK(getStatusFromWriteCommandReply(commandResponse->getCommandReply()));
}

void updateTimestampOnShardCollections(OperationContext* opCtx,
                                       const NamespaceString& nss,
                                       const boost::optional<Timestamp>& timestamp) {
    write_ops::UpdateCommandRequest clearFields(
        NamespaceString::kShardConfigCollectionsNamespace, [&] {
            write_ops::UpdateOpEntry u;
            u.setQ(BSON(ShardCollectionType::kNssFieldName << nss.ns()));
            BSONObj updateOp = (timestamp)
                ? BSON("$set" << BSON(CollectionType::kTimestampFieldName << *timestamp))
                : BSON("$unset" << BSON(CollectionType::kTimestampFieldName << ""));
            u.setU(write_ops::UpdateModification::parseFromClassicUpdate(updateOp));
            return std::vector{u};
        }());

    DBDirectClient client(opCtx);
    const auto commandResult = client.runCommand(clearFields.serialize({}));

    uassertStatusOK(getStatusFromWriteCommandResponse(commandResult->getCommandReply()));
}

Status dropChunksAndDeleteCollectionsEntry(OperationContext* opCtx, const NamespaceString& nss) {
    // TODO (SERVER-58361): Reduce the access to local collections.
    const auto statusWithCollectionEntry = readShardCollectionsEntry(opCtx, nss);
    if (statusWithCollectionEntry.getStatus() == ErrorCodes::NamespaceNotFound) {
        return Status::OK();
    }
    uassertStatusOKWithContext(statusWithCollectionEntry,
                               str::stream() << "Failed to read persisted collection entry for '"
                                             << nss.ns() << "'.");
    const auto& collectionEntry = statusWithCollectionEntry.getValue();

    try {
        DBDirectClient client(opCtx);
        auto deleteCommandResponse = client.runCommand([&] {
            write_ops::DeleteCommandRequest deleteOp(
                NamespaceString::kShardConfigCollectionsNamespace);
            deleteOp.setDeletes({[&] {
                write_ops::DeleteOpEntry entry;
                entry.setQ(BSON(ShardCollectionType::kNssFieldName << nss.ns()));
                entry.setMulti(true);
                return entry;
            }()});
            return deleteOp.serialize({});
        }());
        uassertStatusOK(
            getStatusFromWriteCommandResponse(deleteCommandResponse->getCommandReply()));

        dropChunks(opCtx, nss, collectionEntry.getUuid(), collectionEntry.getSupportingLongName());

        LOGV2(3463200,
              "Dropped chunks and collection caches",
              "collectionNamespace"_attr = nss,
              "collectionUUID"_attr = collectionEntry.getUuid());

        return Status::OK();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

void dropChunks(OperationContext* opCtx,
                const NamespaceString& nss,
                const UUID& uuid,
                SupportingLongNameStatusEnum supportingLongName) {
    const auto chunksNsPostfix{supportingLongName == SupportingLongNameStatusEnum::kDisabled ||
                                       nss.isTemporaryReshardingCollection()
                                   ? nss.ns()
                                   : uuid.toString()};
    const NamespaceString chunksNss{ChunkType::ShardNSPrefix + chunksNsPostfix};

    DBDirectClient client(opCtx);
    BSONObj result;
    if (!client.dropCollection(chunksNss.ns(), kLocalWriteConcern, &result)) {
        auto status = getStatusFromCommandResult(result);
        if (status != ErrorCodes::NamespaceNotFound) {
            uassertStatusOK(status);
        }
    }
}

Status deleteDatabasesEntry(OperationContext* opCtx, StringData dbName) {
    try {
        DBDirectClient client(opCtx);
        auto deleteCommandResponse = client.runCommand([&] {
            write_ops::DeleteCommandRequest deleteOp(
                NamespaceString::kShardConfigDatabasesNamespace);
            deleteOp.setDeletes({[&] {
                write_ops::DeleteOpEntry entry;
                entry.setQ(BSON(ShardDatabaseType::name << dbName.toString()));
                entry.setMulti(false);
                return entry;
            }()});
            return deleteOp.serialize({});
        }());
        uassertStatusOK(
            getStatusFromWriteCommandResponse(deleteCommandResponse->getCommandReply()));

        LOGV2_DEBUG(22092,
                    1,
                    "Successfully cleared persisted metadata for db {db}",
                    "Successfully cleared persisted metadata for db",
                    "db"_attr = dbName);
        return Status::OK();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

}  // namespace shardmetadatautil
}  // namespace mongo
