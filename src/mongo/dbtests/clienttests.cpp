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

#include "mongo/platform/basic.h"

#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/dbtests/dbtests.h"

namespace ClientTests {

using std::string;
using std::unique_ptr;
using std::vector;

class Base {
public:
    Base(string coll) : _ns("test." + coll) {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        DBDirectClient db(&opCtx);

        db.dropDatabase("test");
    }

    virtual ~Base() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        DBDirectClient db(&opCtx);

        db.dropCollection(_ns);
    }

    const NamespaceString nss() {
        return NamespaceString(_ns);
    }

    const char* ns() {
        return _ns.c_str();
    }

    const string _ns;
};


class DropIndex : public Base {
public:
    DropIndex() : Base("dropindex") {}
    void run() {
        // TODO (SERVER-57194): enable lock-free reads.
        bool disableLockFreeReadsOriginalValue = storageGlobalParams.disableLockFreeReads;
        storageGlobalParams.disableLockFreeReads = true;
        ON_BLOCK_EXIT(
            [&] { storageGlobalParams.disableLockFreeReads = disableLockFreeReadsOriginalValue; });

        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        DBDirectClient db(&opCtx);

        const bool includeBuildUUIDs = false;
        const int options = 0;

        db.insert(ns(), BSON("x" << 2));
        ASSERT_EQUALS(1u, db.getIndexSpecs(nss(), includeBuildUUIDs, options).size());

        ASSERT_OK(dbtests::createIndex(&opCtx, ns(), BSON("x" << 1)));
        ASSERT_EQUALS(2u, db.getIndexSpecs(nss(), includeBuildUUIDs, options).size());

        db.dropIndex(ns(), BSON("x" << 1));
        ASSERT_EQUALS(1u, db.getIndexSpecs(nss(), includeBuildUUIDs, options).size());

        ASSERT_OK(dbtests::createIndex(&opCtx, ns(), BSON("x" << 1)));
        ASSERT_EQUALS(2u, db.getIndexSpecs(nss(), includeBuildUUIDs, options).size());

        db.dropIndexes(ns());
        ASSERT_EQUALS(1u, db.getIndexSpecs(nss(), includeBuildUUIDs, options).size());
    }
};

/**
 * Check that nIndexes is incremented correctly when an index builds, and that it is not
 * incremented when an index fails to build.
 */
class BuildIndex : public Base {
public:
    BuildIndex() : Base("buildIndex") {}
    void run() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;

        dbtests::WriteContextForTests ctx(&opCtx, ns());
        DBDirectClient db(&opCtx);

        db.insert(ns(), BSON("x" << 1 << "y" << 2));
        db.insert(ns(), BSON("x" << 2 << "y" << 2));

        const auto& collection = ctx.getCollection();
        ASSERT(collection);
        const IndexCatalog* indexCatalog = collection->getIndexCatalog();

        const bool includeBuildUUIDs = false;
        const int options = 0;

        ASSERT_EQUALS(1, indexCatalog->numIndexesReady(&opCtx));
        // _id index
        ASSERT_EQUALS(1U, db.getIndexSpecs(nss(), includeBuildUUIDs, options).size());

        ASSERT_EQUALS(ErrorCodes::DuplicateKey,
                      dbtests::createIndex(&opCtx, ns(), BSON("y" << 1), true));

        ASSERT_EQUALS(1, indexCatalog->numIndexesReady(&opCtx));
        ASSERT_EQUALS(1U, db.getIndexSpecs(nss(), includeBuildUUIDs, options).size());

        ASSERT_OK(dbtests::createIndex(&opCtx, ns(), BSON("x" << 1), true));

        ASSERT_EQUALS(2, indexCatalog->numIndexesReady(&opCtx));
        ASSERT_EQUALS(2U, db.getIndexSpecs(nss(), includeBuildUUIDs, options).size());
    }
};

class CS_10 : public Base {
public:
    CS_10() : Base("CS_10") {}
    void run() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        DBDirectClient db(&opCtx);

        const string longs(770, 'c');
        for (int i = 0; i < 1111; ++i) {
            db.insert(ns(), BSON("a" << i << "b" << longs));
        }

        ASSERT_OK(dbtests::createIndex(&opCtx, ns(), BSON("a" << 1 << "b" << 1)));

        unique_ptr<DBClientCursor> c =
            db.query(NamespaceString(ns()), BSONObj{}, Query().sort(BSON("a" << 1 << "b" << 1)));
        ASSERT_EQUALS(1111, c->itcount());
    }
};

class PushBack : public Base {
public:
    PushBack() : Base("PushBack") {}
    void run() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        DBDirectClient db(&opCtx);

        for (int i = 0; i < 10; ++i) {
            db.insert(ns(), BSON("i" << i));
        }

        unique_ptr<DBClientCursor> c =
            db.query(NamespaceString(ns()), BSONObj{}, Query().sort(BSON("i" << 1)));

        BSONObj o = c->next();
        ASSERT(c->more());
        ASSERT_EQUALS(9, c->objsLeftInBatch());
        ASSERT(c->moreInCurrentBatch());

        c->putBack(o);
        ASSERT(c->more());
        ASSERT_EQUALS(10, c->objsLeftInBatch());
        ASSERT(c->moreInCurrentBatch());

        o = c->next();
        BSONObj o2 = c->next();
        BSONObj o3 = c->next();
        c->putBack(o3);
        c->putBack(o2);
        c->putBack(o);
        for (int i = 0; i < 10; ++i) {
            o = c->next();
            ASSERT_EQUALS(i, o["i"].number());
        }
        ASSERT(!c->more());
        ASSERT_EQUALS(0, c->objsLeftInBatch());
        ASSERT(!c->moreInCurrentBatch());

        c->putBack(o);
        ASSERT(c->more());
        ASSERT_EQUALS(1, c->objsLeftInBatch());
        ASSERT(c->moreInCurrentBatch());
        ASSERT_EQUALS(1, c->itcount());
    }
};

class Create : public Base {
public:
    Create() : Base("Create") {}
    void run() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        DBDirectClient db(&opCtx);

        db.createCollection("unittests.clienttests.create");
        BSONObj info;
        ASSERT(db.runCommand("unittests",
                             BSON("collstats"
                                  << "clienttests.create"),
                             info));
    }
};

class ConnectionStringTests {
public:
    void run() {
        {
            ConnectionString s("a/b,c,d", ConnectionString::ConnectionType::kReplicaSet);
            ASSERT_EQUALS(ConnectionString::ConnectionType::kReplicaSet, s.type());
            ASSERT_EQUALS("a", s.getSetName());
            vector<HostAndPort> v = s.getServers();
            ASSERT_EQUALS(3U, v.size());
            ASSERT_EQUALS("b", v[0].host());
            ASSERT_EQUALS("c", v[1].host());
            ASSERT_EQUALS("d", v[2].host());
        }
    }
};

class CreateSimpleV1Index : public Base {
public:
    CreateSimpleV1Index() : Base("CreateSimpleV1Index") {}
    void run() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        DBDirectClient db(&opCtx);

        db.createIndex(ns(), IndexSpec().addKey("aField").version(1));
    }
};

class CreateSimpleNamedV1Index : public Base {
public:
    CreateSimpleNamedV1Index() : Base("CreateSimpleNamedV1Index") {}
    void run() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        DBDirectClient db(&opCtx);

        db.createIndex(ns(), IndexSpec().addKey("aField").version(1).name("aFieldV1Index"));
    }
};

class CreateCompoundNamedV1Index : public Base {
public:
    CreateCompoundNamedV1Index() : Base("CreateCompoundNamedV1Index") {}
    void run() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        DBDirectClient db(&opCtx);

        db.createIndex(ns(),
                       IndexSpec()
                           .addKey("aField")
                           .addKey("bField", IndexSpec::kIndexTypeDescending)
                           .version(1)
                           .name("aFieldbFieldV1Index"));
    }
};

class CreateUniqueSparseDropDupsIndexInBackground : public Base {
public:
    CreateUniqueSparseDropDupsIndexInBackground()
        : Base("CreateUniqueSparseDropDupsIndexInBackground") {}
    void run() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        DBDirectClient db(&opCtx);

        db.createIndex(
            ns(), IndexSpec().addKey("aField").background().unique().sparse().dropDuplicates());
    }
};

class CreateComplexTextIndex : public Base {
public:
    CreateComplexTextIndex() : Base("CreateComplexTextIndex") {}
    void run() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        DBDirectClient db(&opCtx);

        db.createIndex(ns(),
                       IndexSpec()
                           .addKey("aField", IndexSpec::kIndexTypeText)
                           .addKey("bField", IndexSpec::kIndexTypeText)
                           .textWeights(BSON("aField" << 100))
                           .textDefaultLanguage("spanish")
                           .textLanguageOverride("lang")
                           .textIndexVersion(2));
    }
};

class Create2DIndex : public Base {
public:
    Create2DIndex() : Base("Create2DIndex") {}
    void run() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        DBDirectClient db(&opCtx);

        db.createIndex(ns(),
                       IndexSpec()
                           .addKey("aField", IndexSpec::kIndexTypeGeo2D)
                           .geo2DBits(20)
                           .geo2DMin(-120.0)
                           .geo2DMax(120.0));
    }
};

class Create2DSphereIndex : public Base {
public:
    Create2DSphereIndex() : Base("Create2DSphereIndex") {}
    void run() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        DBDirectClient db(&opCtx);

        db.createIndex(ns(),
                       IndexSpec()
                           .addKey("aField", IndexSpec::kIndexTypeGeo2DSphere)
                           .geo2DSphereIndexVersion(2));
    }
};

class CreateHashedIndex : public Base {
public:
    CreateHashedIndex() : Base("CreateHashedIndex") {}
    void run() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        DBDirectClient db(&opCtx);

        db.createIndex(ns(), IndexSpec().addKey("aField", IndexSpec::kIndexTypeHashed));
    }
};

class CreateIndexFailure : public Base {
public:
    CreateIndexFailure() : Base("CreateIndexFailure") {}
    void run() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        DBDirectClient db(&opCtx);

        db.createIndex(ns(), IndexSpec().addKey("aField"));
        ASSERT_THROWS(db.createIndex(ns(), IndexSpec().addKey("aField").unique()),
                      AssertionException);
    }
};

class All : public OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("client") {}

    void setupTests() {
        add<DropIndex>();
        add<BuildIndex>();
        add<CS_10>();
        add<PushBack>();
        add<Create>();
        add<ConnectionStringTests>();
        add<CreateSimpleV1Index>();
        add<CreateSimpleNamedV1Index>();
        add<CreateCompoundNamedV1Index>();
        add<CreateUniqueSparseDropDupsIndexInBackground>();
        add<CreateComplexTextIndex>();
        add<Create2DIndex>();
        add<Create2DSphereIndex>();
        add<CreateHashedIndex>();
        add<CreateIndexFailure>();
    }
};

OldStyleSuiteInitializer<All> all;
}  // namespace ClientTests
