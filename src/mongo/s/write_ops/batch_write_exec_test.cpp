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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/s/sharding_router_test_fixture.h"
#include "mongo/s/stale_exception.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/write_ops/batch_write_exec.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/mock_ns_targeter.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using executor::RemoteCommandRequest;

const HostAndPort kTestConfigShardHost = HostAndPort("FakeConfigHost", 12345);

const HostAndPort kTestShardHost1 = HostAndPort("FakeHost1", 12345);
const std::string kShardName1 = "FakeShard1";
const HostAndPort kTestShardHost2 = HostAndPort("FakeHost2", 12345);
const std::string kShardName2 = "FakeShard2";

const int kMaxRoundsWithoutProgress = 5;

BSONObj expectInsertsReturnStaleVersionErrorsBase(const NamespaceString& nss,
                                                  const std::vector<BSONObj>& expected,
                                                  const executor::RemoteCommandRequest& request) {
    ASSERT_EQUALS(nss.db(), request.dbname);

    const auto opMsgRequest(OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj));
    const auto actualBatchedInsert(BatchedCommandRequest::parseInsert(opMsgRequest));
    ASSERT_EQUALS(nss.toString(), actualBatchedInsert.getNS().ns());

    const auto& inserted = actualBatchedInsert.getInsertRequest().getDocuments();
    ASSERT_EQUALS(expected.size(), inserted.size());

    auto itInserted = inserted.begin();
    auto itExpected = expected.begin();

    for (; itInserted != inserted.end(); itInserted++, itExpected++) {
        ASSERT_BSONOBJ_EQ(*itExpected, *itInserted);
    }

    BatchedCommandResponse staleResponse;
    staleResponse.setStatus(Status::OK());
    staleResponse.setN(0);

    auto epoch = OID::gen();

    // Report a stale version error for each write in the batch.
    int i = 0;
    for (itInserted = inserted.begin(); itInserted != inserted.end(); ++itInserted) {
        WriteErrorDetail* error = new WriteErrorDetail;
        error->setStatus({ErrorCodes::StaleShardVersion, "mock stale error"});
        error->setErrInfo([&] {
            StaleConfigInfo sci(nss, ChunkVersion(1, 0, epoch), ChunkVersion(2, 0, epoch));
            BSONObjBuilder builder;
            sci.serialize(&builder);
            return builder.obj();
        }());
        error->setIndex(i);

        staleResponse.addToErrDetails(error);
        ++i;
    }

    return staleResponse.toBSON();
}

/**
 * Mimics a single shard backend for a particular collection which can be initialized with a
 * set of write command results to return.
 */
class BatchWriteExecTest : public ShardingTestFixture {
public:
    BatchWriteExecTest() = default;
    ~BatchWriteExecTest() = default;

    void setUp() override {
        ShardingTestFixture::setUp();
        setRemote(HostAndPort("ClientHost", 12345));

        // Set up the RemoteCommandTargeter for the config shard
        configTargeter()->setFindHostReturnValue(kTestConfigShardHost);
        // Add a RemoteCommandTargeter for the data shard2
        targeterFactory()->addTargeterToReturn(ConnectionString(kTestShardHost1), [] {
            std::unique_ptr<RemoteCommandTargeterMock> targeter(
                std::make_unique<RemoteCommandTargeterMock>());
            targeter->setConnectionStringReturnValue(ConnectionString(kTestShardHost1));
            targeter->setFindHostReturnValue(kTestShardHost1);
            return targeter;
        }());

        targeterFactory()->addTargeterToReturn(ConnectionString(kTestShardHost2), [] {
            std::unique_ptr<RemoteCommandTargeterMock> targeter(
                std::make_unique<RemoteCommandTargeterMock>());
            targeter->setConnectionStringReturnValue(ConnectionString(kTestShardHost2));
            targeter->setFindHostReturnValue(kTestShardHost2);
            return targeter;
        }());

        // Set up the shard registry to contain the fake shards
        setupShards({[] {
                         ShardType shardType;
                         shardType.setName(kShardName1);
                         shardType.setHost(kTestShardHost1.toString());
                         return shardType;
                     }(),
                     [] {
                         ShardType shardType;
                         shardType.setName(kShardName2);
                         shardType.setHost(kTestShardHost2.toString());
                         return shardType;
                     }()});
    }

    void expectInsertsReturnSuccess(const std::vector<BSONObj>& expected) {
        expectInsertsReturnSuccess(expected.begin(), expected.end());
    }

    void expectInsertsReturnSuccess(std::vector<BSONObj>::const_iterator expectedFrom,
                                    std::vector<BSONObj>::const_iterator expectedTo) {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQUALS(nss.db(), request.dbname);

            const auto opMsgRequest(OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj));
            const auto actualBatchedInsert(BatchedCommandRequest::parseInsert(opMsgRequest));
            ASSERT_EQUALS(nss.toString(), actualBatchedInsert.getNS().ns());

            const auto& inserted = actualBatchedInsert.getInsertRequest().getDocuments();
            const size_t expectedSize = std::distance(expectedFrom, expectedTo);
            ASSERT_EQUALS(expectedSize, inserted.size());

            auto itInserted = inserted.begin();
            auto itExpected = expectedFrom;

            for (; itInserted != inserted.end(); itInserted++, itExpected++) {
                ASSERT_BSONOBJ_EQ(*itExpected, *itInserted);
            }

            BatchedCommandResponse response;
            response.setStatus(Status::OK());
            response.setN(inserted.size());

            return response.toBSON();
        });
    }

    void expectInsertsReturnStaleVersionErrors(const std::vector<BSONObj>& expected) {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            return expectInsertsReturnStaleVersionErrorsBase(nss, expected, request);
        });
    }

    void expectInsertsReturnError(const std::vector<BSONObj>& expected,
                                  const BatchedCommandResponse& errResponse) {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            try {
                ASSERT_EQUALS(nss.db(), request.dbname);

                const auto opMsgRequest(
                    OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj));
                const auto actualBatchedInsert(BatchedCommandRequest::parseInsert(opMsgRequest));
                ASSERT_EQUALS(nss.toString(), actualBatchedInsert.getNS().ns());

                const auto& inserted = actualBatchedInsert.getInsertRequest().getDocuments();
                ASSERT_EQUALS(expected.size(), inserted.size());

                auto itInserted = inserted.begin();
                auto itExpected = expected.begin();

                for (; itInserted != inserted.end(); itInserted++, itExpected++) {
                    ASSERT_BSONOBJ_EQ(*itExpected, *itInserted);
                }

                return errResponse.toBSON();
            } catch (const DBException& ex) {
                BSONObjBuilder bb;
                CommandHelpers::appendCommandStatusNoThrow(bb, ex.toStatus());
                return bb.obj();
            }
        });
    }

    const NamespaceString nss{"foo.bar"};

    MockNSTargeter singleShardNSTargeter{
        nss,
        {MockRange(ShardEndpoint(kShardName1, ChunkVersion(100, 200, OID::gen())),
                   BSON("x" << MINKEY),
                   BSON("x" << MAXKEY))}};
};

//
// Tests for the BatchWriteExec
//

TEST_F(BatchWriteExecTest, SingleOp) {
    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        insertOp.setDocuments({BSON("x" << 1)});
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    // Do single-target, single doc batch write op
    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);
        ASSERT(response.getOk());
        ASSERT_EQ(1LL, response.getN());
        ASSERT_EQ(1, stats.numRounds);
    });

    expectInsertsReturnSuccess(std::vector<BSONObj>{BSON("x" << 1)});

    future.default_timed_get();
}

TEST_F(BatchWriteExecTest, MultiOpLarge) {
    const int kNumDocsToInsert = 100'000;
    const std::string kDocValue(200, 'x');

    std::vector<BSONObj> docsToInsert;
    docsToInsert.reserve(kNumDocsToInsert);
    for (int i = 0; i < kNumDocsToInsert; i++) {
        docsToInsert.push_back(BSON("_id" << i << "someLargeKeyToWasteSpace" << kDocValue));
    }

    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase writeCommandBase;
            writeCommandBase.setOrdered(true);
            return writeCommandBase;
        }());
        insertOp.setDocuments(docsToInsert);
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);

        ASSERT(response.getOk());
        ASSERT_EQUALS(response.getN(), kNumDocsToInsert);
        ASSERT_EQUALS(stats.numRounds, 2);
    });

    expectInsertsReturnSuccess(docsToInsert.begin(), docsToInsert.begin() + 66576);
    expectInsertsReturnSuccess(docsToInsert.begin() + 66576, docsToInsert.end());

    future.default_timed_get();
}

TEST_F(BatchWriteExecTest, SingleOpError) {
    BatchedCommandResponse errResponse;
    errResponse.setStatus({ErrorCodes::UnknownError, "mock error"});

    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        insertOp.setDocuments({BSON("x" << 1)});
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);
        ASSERT(response.getOk());
        ASSERT_EQ(0, response.getN());
        ASSERT(response.isErrDetailsSet());
        ASSERT_EQ(errResponse.toStatus().code(), response.getErrDetailsAt(0)->toStatus().code());
        ASSERT(response.getErrDetailsAt(0)->toStatus().reason().find(
                   errResponse.toStatus().reason()) != std::string::npos);

        ASSERT_EQ(1, stats.numRounds);
    });

    expectInsertsReturnError({BSON("x" << 1)}, errResponse);

    future.default_timed_get();
}

TEST_F(BatchWriteExecTest, StaleShardVersionReturnedFromBatchWithSingleMultiWrite) {
    BatchedCommandRequest request([&] {
        write_ops::Update updateOp(nss);
        updateOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        updateOp.setUpdates(
            std::vector{write_ops::UpdateOpEntry(BSON("_id" << 100), {BSON("Key" << 100)})});
        return updateOp;
    }());
    request.setWriteConcern(BSONObj());

    static const auto epoch = OID::gen();

    class MultiShardTargeter : public MockNSTargeter {
    public:
        using MockNSTargeter::MockNSTargeter;

        StatusWith<std::vector<ShardEndpoint>> targetUpdate(
            OperationContext* opCtx, const write_ops::UpdateOpEntry& updateDoc) const override {
            return std::vector<ShardEndpoint>{
                ShardEndpoint(kShardName1, ChunkVersion(100, 200, epoch)),
                ShardEndpoint(kShardName2, ChunkVersion(101, 200, epoch))};
        }
    };

    MultiShardTargeter multiShardNSTargeter(
        nss,
        {MockRange(ShardEndpoint(kShardName1, ChunkVersion(100, 200, epoch)),
                   BSON("x" << MINKEY),
                   BSON("x" << 0)),
         MockRange(ShardEndpoint(kShardName2, ChunkVersion(101, 200, epoch)),
                   BSON("x" << 0),
                   BSON("x" << MAXKEY))});

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), multiShardNSTargeter, request, &response, &stats);

        return response;
    });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(kTestShardHost1, request.target);

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(1);

        return response.toBSON();
    });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(kTestShardHost2, request.target);

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(0);
        response.addToErrDetails([&] {
            WriteErrorDetail* errDetail = new WriteErrorDetail();
            errDetail->setIndex(0);
            errDetail->setStatus({ErrorCodes::StaleShardVersion, "Stale shard version"});
            errDetail->setErrInfo([&] {
                Status ssvStatus(StaleConfigInfo(nss,
                                                 ChunkVersion(101, 200, epoch),
                                                 ChunkVersion(105, 200, epoch)),
                                 "Stale shard version");
                BSONObjBuilder builder;
                ssvStatus.serializeErrorToBSON(&builder);
                return builder.obj();
            }());
            return errDetail;
        }());

        return response.toBSON();
    });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(kTestShardHost2, request.target);

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(2);

        return response.toBSON();
    });

    auto response = future.default_timed_get();
    ASSERT_OK(response.getTopLevelStatus());
    ASSERT_EQ(3, response.getNModified());
}

TEST_F(BatchWriteExecTest,
       RetryableErrorReturnedFromMultiWriteWithShard1AllOKShard2AllStaleShardVersion) {
    BatchedCommandRequest request([&] {
        write_ops::Update updateOp(nss);
        updateOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        updateOp.setUpdates(
            std::vector{write_ops::UpdateOpEntry(BSON("id" << 150), {BSON("x" << 1)}),
                        write_ops::UpdateOpEntry(BSON("id" << 200), {BSON("y" << 2)})});
        return updateOp;
    }());
    request.setWriteConcern(BSONObj());

    static const auto epoch = OID::gen();

    // This allows the batch to target each write operation
    // to a specific shard (kShardName2), to perform this test
    class MultiShardTargeter : public MockNSTargeter {
    public:
        using MockNSTargeter::MockNSTargeter;

        StatusWith<std::vector<ShardEndpoint>> targetUpdate(
            OperationContext* opCtx, const write_ops::UpdateOpEntry& updateDoc) const override {
            return std::vector<ShardEndpoint>{
                ShardEndpoint(kShardName1, ChunkVersion(100, 200, epoch)),
                ShardEndpoint(kShardName2, ChunkVersion(101, 200, epoch))};
        }
    };

    MultiShardTargeter multiShardNSTargeter(
        nss,
        {MockRange(ShardEndpoint(kShardName1, ChunkVersion(100, 200, epoch)),
                   BSON("sk" << MINKEY),
                   BSON("sk" << 10)),
         MockRange(ShardEndpoint(kShardName2, ChunkVersion(101, 200, epoch)),
                   BSON("sk" << 10),
                   BSON("sk" << MAXKEY))});

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), multiShardNSTargeter, request, &response, &stats);

        return response;
    });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(kTestShardHost1, request.target);

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(1);

        return response.toBSON();
    });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(kTestShardHost2, request.target);

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(0);
        response.addToErrDetails([&] {
            WriteErrorDetail* errDetail = new WriteErrorDetail();
            errDetail->setIndex(0);
            errDetail->setStatus({ErrorCodes::StaleShardVersion, "Stale shard version"});
            errDetail->setErrInfo([&] {
                Status ssvStatus(StaleConfigInfo(nss,
                                                 ChunkVersion(101, 200, epoch),
                                                 ChunkVersion(105, 200, epoch)),
                                 "Stale shard version");
                BSONObjBuilder builder;
                ssvStatus.serializeErrorToBSON(&builder);
                return builder.obj();
            }());
            return errDetail;
        }());
        response.addToErrDetails([&] {
            WriteErrorDetail* errDetail = new WriteErrorDetail();
            errDetail->setIndex(1);
            errDetail->setStatus({ErrorCodes::StaleShardVersion, "Stale shard version"});
            errDetail->setErrInfo([&] {
                Status ssvStatus(StaleConfigInfo(nss,
                                                 ChunkVersion(101, 200, epoch),
                                                 ChunkVersion(105, 200, epoch)),
                                 "Stale shard version");
                BSONObjBuilder builder;
                ssvStatus.serializeErrorToBSON(&builder);
                return builder.obj();
            }());
            return errDetail;
        }());

        return response.toBSON();
    });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(kTestShardHost2, request.target);

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(2);

        return response.toBSON();
    });

    auto response = future.default_timed_get();
    ASSERT_OK(response.getTopLevelStatus());
    ASSERT_EQ(3, response.getNModified());
}

TEST_F(BatchWriteExecTest, RetryableErrorReturnedFromMultiWriteWithShard1Firs) {
    BatchedCommandRequest request([&] {
        write_ops::Update updateOp(nss);
        updateOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        updateOp.setUpdates(
            std::vector{write_ops::UpdateOpEntry(BSON("id" << 150), {BSON("x" << 1)}),
                        write_ops::UpdateOpEntry(BSON("id" << 200), {BSON("y" << 2)})});
        return updateOp;
    }());
    request.setWriteConcern(BSONObj());

    static const auto epoch = OID::gen();

    // This allows the batch to target each write operation
    // to a specific shard (kShardName2), to perform this test
    class MultiShardTargeter : public MockNSTargeter {
    public:
        using MockNSTargeter::MockNSTargeter;

        StatusWith<std::vector<ShardEndpoint>> targetUpdate(
            OperationContext* opCtx, const write_ops::UpdateOpEntry& updateDoc) const override {
            return std::vector<ShardEndpoint>{
                ShardEndpoint(kShardName1, ChunkVersion(100, 200, epoch)),
                ShardEndpoint(kShardName2, ChunkVersion(101, 200, epoch))};
        }
    };

    MultiShardTargeter multiShardNSTargeter(
        nss,
        {MockRange(ShardEndpoint(kShardName1, ChunkVersion(100, 200, epoch)),
                   BSON("sk" << MINKEY),
                   BSON("sk" << 10)),
         MockRange(ShardEndpoint(kShardName2, ChunkVersion(101, 200, epoch)),
                   BSON("sk" << 10),
                   BSON("sk" << MAXKEY))});

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), multiShardNSTargeter, request, &response, &stats);

        return response;
    });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(kTestShardHost1, request.target);

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(0);
        response.addToErrDetails([&] {
            WriteErrorDetail* errDetail = new WriteErrorDetail();
            errDetail->setIndex(1);
            errDetail->setStatus({ErrorCodes::StaleShardVersion, "Stale shard version"});
            errDetail->setErrInfo([&] {
                Status ssvStatus(StaleConfigInfo(nss,
                                                 ChunkVersion(101, 200, epoch),
                                                 ChunkVersion(105, 200, epoch)),
                                 "Stale shard version");
                BSONObjBuilder builder;
                ssvStatus.serializeErrorToBSON(&builder);
                return builder.obj();
            }());
            return errDetail;
        }());

        return response.toBSON();
    });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(kTestShardHost2, request.target);

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(0);
        response.addToErrDetails([&] {
            WriteErrorDetail* errDetail = new WriteErrorDetail();
            errDetail->setIndex(0);
            errDetail->setStatus({ErrorCodes::StaleShardVersion, "Stale shard version"});
            errDetail->setErrInfo([&] {
                Status ssvStatus(StaleConfigInfo(nss,
                                                 ChunkVersion(101, 200, epoch),
                                                 ChunkVersion(105, 200, epoch)),
                                 "Stale shard version");
                BSONObjBuilder builder;
                ssvStatus.serializeErrorToBSON(&builder);
                return builder.obj();
            }());
            return errDetail;
        }());

        return response.toBSON();
    });


    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(kTestShardHost1, request.target);

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(1);

        return response.toBSON();
    });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(kTestShardHost2, request.target);

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(1);

        return response.toBSON();
    });

    auto response = future.default_timed_get();
    ASSERT_OK(response.getTopLevelStatus());
    ASSERT_EQ(2, response.getNModified());
}

TEST_F(BatchWriteExecTest, RetryableErrorReturnedFromMultiWriteWithShard1FirstOKShard2FirstOK) {
    BatchedCommandRequest request([&] {
        write_ops::Update updateOp(nss);
        updateOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        updateOp.setUpdates(
            std::vector{write_ops::UpdateOpEntry(BSON("id" << 150), {BSON("x" << 1)}),
                        write_ops::UpdateOpEntry(BSON("id" << 200), {BSON("y" << 2)})});
        return updateOp;
    }());
    request.setWriteConcern(BSONObj());

    static const auto epoch = OID::gen();

    // This allows the batch to target each write operation
    // to a specific shard (kShardName2), to perform this test
    class MultiShardTargeter : public MockNSTargeter {
    public:
        using MockNSTargeter::MockNSTargeter;

        StatusWith<std::vector<ShardEndpoint>> targetUpdate(
            OperationContext* opCtx, const write_ops::UpdateOpEntry& updateDoc) const override {
            return std::vector<ShardEndpoint>{
                ShardEndpoint(kShardName1, ChunkVersion(100, 200, epoch)),
                ShardEndpoint(kShardName2, ChunkVersion(101, 200, epoch))};
        }
    };

    MultiShardTargeter multiShardNSTargeter(
        nss,
        {MockRange(ShardEndpoint(kShardName1, ChunkVersion(100, 200, epoch)),
                   BSON("sk" << MINKEY),
                   BSON("sk" << 10)),
         MockRange(ShardEndpoint(kShardName2, ChunkVersion(101, 200, epoch)),
                   BSON("sk" << 10),
                   BSON("sk" << MAXKEY))});

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), multiShardNSTargeter, request, &response, &stats);

        return response;
    });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(kTestShardHost1, request.target);

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(0);
        response.addToErrDetails([&] {
            WriteErrorDetail* errDetail = new WriteErrorDetail();
            errDetail->setIndex(1);
            errDetail->setStatus({ErrorCodes::StaleShardVersion, "Stale shard version"});
            errDetail->setErrInfo([&] {
                Status ssvStatus(StaleConfigInfo(nss,
                                                 ChunkVersion(101, 200, epoch),
                                                 ChunkVersion(105, 200, epoch)),
                                 "Stale shard version");
                BSONObjBuilder builder;
                ssvStatus.serializeErrorToBSON(&builder);
                return builder.obj();
            }());
            return errDetail;
        }());

        return response.toBSON();
    });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(kTestShardHost2, request.target);

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(0);
        response.addToErrDetails([&] {
            WriteErrorDetail* errDetail = new WriteErrorDetail();
            errDetail->setIndex(1);
            errDetail->setStatus({ErrorCodes::StaleShardVersion, "Stale shard version"});
            errDetail->setErrInfo([&] {
                Status ssvStatus(StaleConfigInfo(nss,
                                                 ChunkVersion(101, 200, epoch),
                                                 ChunkVersion(105, 200, epoch)),
                                 "Stale shard version");
                BSONObjBuilder builder;
                ssvStatus.serializeErrorToBSON(&builder);
                return builder.obj();
            }());
            return errDetail;
        }());

        return response.toBSON();
    });


    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(kTestShardHost1, request.target);

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(1);

        return response.toBSON();
    });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(kTestShardHost2, request.target);

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(1);

        return response.toBSON();
    });

    auto response = future.default_timed_get();
    ASSERT_OK(response.getTopLevelStatus());
    ASSERT_EQ(2, response.getNModified());
}

//
// Test retryable errors
//

TEST_F(BatchWriteExecTest, StaleOp) {
    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        insertOp.setDocuments({BSON("x" << 1)});
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    // Execute request
    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);
        ASSERT(response.getOk());

        ASSERT_EQUALS(1, stats.numStaleBatches);
    });

    const std::vector<BSONObj> expected{BSON("x" << 1)};

    expectInsertsReturnStaleVersionErrors(expected);
    expectInsertsReturnSuccess(expected);

    future.default_timed_get();
}

TEST_F(BatchWriteExecTest, MultiStaleOp) {
    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        insertOp.setDocuments({BSON("x" << 1)});
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);
        ASSERT(response.getOk());

        ASSERT_EQUALS(3, stats.numStaleBatches);
    });

    const std::vector<BSONObj> expected{BSON("x" << 1)};

    // Return multiple StaleShardVersion errors, but less than the give-up number
    for (int i = 0; i < 3; i++) {
        expectInsertsReturnStaleVersionErrors(expected);
    }

    expectInsertsReturnSuccess(expected);

    future.default_timed_get();
}

TEST_F(BatchWriteExecTest, TooManyStaleShardOp) {
    // Retry op in exec too many times (without refresh) b/c of stale config (the mock
    // singleShardNSTargeter doesn't report progress on refresh). We should report a no progress
    // error for everything in the batch.

    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        insertOp.setDocuments({BSON("x" << 1), BSON("x" << 2)});
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);
        ASSERT(response.getOk());
        ASSERT_EQ(0, response.getN());
        ASSERT(response.isErrDetailsSet());
        ASSERT_EQUALS(response.getErrDetailsAt(0)->toStatus().code(), ErrorCodes::NoProgressMade);
        ASSERT_EQUALS(response.getErrDetailsAt(1)->toStatus().code(), ErrorCodes::NoProgressMade);

        ASSERT_EQUALS(stats.numStaleBatches, (1 + kMaxRoundsWithoutProgress));
    });

    // Return multiple StaleShardVersion errors
    for (int i = 0; i < (1 + kMaxRoundsWithoutProgress); i++) {
        expectInsertsReturnStaleVersionErrors({BSON("x" << 1), BSON("x" << 2)});
    }

    future.default_timed_get();
}

TEST_F(BatchWriteExecTest, RetryableWritesLargeBatch) {
    // A retryable error without a txnNumber is not retried.

    const int kNumDocsToInsert = 100'000;
    const std::string kDocValue(200, 'x');

    std::vector<BSONObj> docsToInsert;
    docsToInsert.reserve(kNumDocsToInsert);
    for (int i = 0; i < kNumDocsToInsert; i++) {
        docsToInsert.push_back(BSON("_id" << i << "someLargeKeyToWasteSpace" << kDocValue));
    }

    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase writeCommandBase;
            writeCommandBase.setOrdered(true);
            return writeCommandBase;
        }());
        insertOp.setDocuments(docsToInsert);
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    operationContext()->setLogicalSessionId(makeLogicalSessionIdForTest());
    operationContext()->setTxnNumber(5);

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);

        ASSERT(response.getOk());
        ASSERT_EQUALS(response.getN(), kNumDocsToInsert);
        ASSERT_EQUALS(stats.numRounds, 2);
    });

    expectInsertsReturnSuccess(docsToInsert.begin(), docsToInsert.begin() + 63791);
    expectInsertsReturnSuccess(docsToInsert.begin() + 63791, docsToInsert.end());

    future.default_timed_get();
}

TEST_F(BatchWriteExecTest, RetryableErrorNoTxnNumber) {
    // A retryable error without a txnNumber is not retried.

    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase writeCommandBase;
            writeCommandBase.setOrdered(true);
            return writeCommandBase;
        }());
        insertOp.setDocuments({BSON("x" << 1), BSON("x" << 2)});
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    BatchedCommandResponse retryableErrResponse;
    retryableErrResponse.setStatus({ErrorCodes::NotMaster, "mock retryable error"});

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);

        ASSERT(response.getOk());
        ASSERT_EQ(0, response.getN());
        ASSERT(response.isErrDetailsSet());
        ASSERT_EQUALS(response.getErrDetailsAt(0)->toStatus().code(),
                      retryableErrResponse.toStatus().code());
        ASSERT(response.getErrDetailsAt(0)->toStatus().reason().find(
                   retryableErrResponse.toStatus().reason()) != std::string::npos);
        ASSERT_EQ(1, stats.numRounds);
    });

    expectInsertsReturnError({BSON("x" << 1), BSON("x" << 2)}, retryableErrResponse);

    future.default_timed_get();
}

TEST_F(BatchWriteExecTest, RetryableErrorTxnNumber) {
    // A retryable error with a txnNumber is automatically retried.

    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase writeCommandBase;
            writeCommandBase.setOrdered(true);
            return writeCommandBase;
        }());
        insertOp.setDocuments({BSON("x" << 1), BSON("x" << 2)});
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    operationContext()->setLogicalSessionId(makeLogicalSessionIdForTest());
    operationContext()->setTxnNumber(5);

    BatchedCommandResponse retryableErrResponse;
    retryableErrResponse.setStatus({ErrorCodes::NotMaster, "mock retryable error"});

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);

        ASSERT(response.getOk());
        ASSERT(!response.isErrDetailsSet());
        ASSERT_EQ(1, stats.numRounds);
    });

    expectInsertsReturnError({BSON("x" << 1), BSON("x" << 2)}, retryableErrResponse);
    expectInsertsReturnSuccess({BSON("x" << 1), BSON("x" << 2)});

    future.default_timed_get();
}

TEST_F(BatchWriteExecTest, NonRetryableErrorTxnNumber) {
    // A non-retryable error with a txnNumber is not retried.

    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase writeCommandBase;
            writeCommandBase.setOrdered(true);
            return writeCommandBase;
        }());
        insertOp.setDocuments({BSON("x" << 1), BSON("x" << 2)});
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    operationContext()->setLogicalSessionId(makeLogicalSessionIdForTest());
    operationContext()->setTxnNumber(5);

    BatchedCommandResponse nonRetryableErrResponse;
    nonRetryableErrResponse.setStatus({ErrorCodes::UnknownError, "mock non-retryable error"});

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);

        ASSERT(response.getOk());
        ASSERT_EQ(0, response.getN());
        ASSERT(response.isErrDetailsSet());
        ASSERT_EQUALS(response.getErrDetailsAt(0)->toStatus().code(),
                      nonRetryableErrResponse.toStatus().code());
        ASSERT(response.getErrDetailsAt(0)->toStatus().reason().find(
                   nonRetryableErrResponse.toStatus().reason()) != std::string::npos);
        ASSERT_EQ(1, stats.numRounds);
    });

    expectInsertsReturnError({BSON("x" << 1), BSON("x" << 2)}, nonRetryableErrResponse);

    future.default_timed_get();
}

TEST_F(BatchWriteExecTest, StaleEpochIsNotRetryable) {
    // A StaleEpoch error is not retried.

    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase writeCommandBase;
            writeCommandBase.setOrdered(true);
            return writeCommandBase;
        }());
        insertOp.setDocuments({BSON("x" << 1), BSON("x" << 2)});
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    operationContext()->setLogicalSessionId(makeLogicalSessionIdForTest());
    operationContext()->setTxnNumber(5);

    BatchedCommandResponse nonRetryableErrResponse;
    nonRetryableErrResponse.setStatus({ErrorCodes::StaleEpoch, "mock stale epoch error"});

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);
        ASSERT(response.getOk());
        ASSERT_EQ(0, response.getN());
        ASSERT(response.isErrDetailsSet());
        ASSERT_EQUALS(response.getErrDetailsAt(0)->toStatus().code(),
                      nonRetryableErrResponse.toStatus().code());
        ASSERT(response.getErrDetailsAt(0)->toStatus().reason().find(
                   nonRetryableErrResponse.toStatus().reason()) != std::string::npos);
        ASSERT_EQ(1, stats.numRounds);
    });

    expectInsertsReturnError({BSON("x" << 1), BSON("x" << 2)}, nonRetryableErrResponse);

    future.default_timed_get();
}

class BatchWriteExecTransactionTest : public BatchWriteExecTest {
public:
    const TxnNumber kTxnNumber = 5;
    const LogicalTime kInMemoryLogicalTime = LogicalTime(Timestamp(3, 1));

    void setUp() override {
        BatchWriteExecTest::setUp();

        operationContext()->setLogicalSessionId(makeLogicalSessionIdForTest());
        operationContext()->setTxnNumber(kTxnNumber);
        repl::ReadConcernArgs::get(operationContext()) =
            repl::ReadConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern);

        auto logicalClock = stdx::make_unique<LogicalClock>(getServiceContext());
        logicalClock->setClusterTimeFromTrustedSource(kInMemoryLogicalTime);
        LogicalClock::set(getServiceContext(), std::move(logicalClock));

        _scopedSession.emplace(operationContext());

        auto txnRouter = TransactionRouter::get(operationContext());
        txnRouter.beginOrContinueTxn(
            operationContext(), kTxnNumber, TransactionRouter::TransactionActions::kStart);
        txnRouter.setDefaultAtClusterTime(operationContext());
    }

    void tearDown() override {
        _scopedSession.reset();
        repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();

        BatchWriteExecTest::tearDown();
    }

    void expectInsertsReturnStaleVersionErrors(const std::vector<BSONObj>& expected) {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            BSONObjBuilder bob;

            bob.appendElementsUnique(
                expectInsertsReturnStaleVersionErrorsBase(nss, expected, request));

            // Because this is the transaction-specific fixture, return transaction metadata in
            // the response.
            TxnResponseMetadata txnResponseMetadata(false /* readOnly */);
            txnResponseMetadata.serialize(&bob);

            return bob.obj();
        });
    }

    void expectInsertsReturnTransientTxnErrors(const std::vector<BSONObj>& expected) {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQUALS(nss.db(), request.dbname);

            const auto opMsgRequest(OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj));
            const auto actualBatchedInsert(BatchedCommandRequest::parseInsert(opMsgRequest));
            ASSERT_EQUALS(nss.toString(), actualBatchedInsert.getNS().ns());

            const auto& inserted = actualBatchedInsert.getInsertRequest().getDocuments();
            ASSERT_EQUALS(expected.size(), inserted.size());

            auto itInserted = inserted.begin();
            auto itExpected = expected.begin();

            for (; itInserted != inserted.end(); itInserted++, itExpected++) {
                ASSERT_BSONOBJ_EQ(*itExpected, *itInserted);
            }

            BSONObjBuilder bob;

            bob.append("ok", 0);
            bob.append("errorLabels", BSON_ARRAY("TransientTransactionError"));
            bob.append("code", ErrorCodes::WriteConflict);
            bob.append("codeName", ErrorCodes::errorString(ErrorCodes::WriteConflict));

            // Because this is the transaction-specific fixture, return transaction metadata in
            // the response.
            TxnResponseMetadata txnResponseMetadata(false /* readOnly */);
            txnResponseMetadata.serialize(&bob);

            return bob.obj();
        });
    }

private:
    boost::optional<RouterOperationContextSession> _scopedSession;
};

TEST_F(BatchWriteExecTransactionTest, ErrorInBatchThrows_CommandError) {
    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        insertOp.setDocuments({BSON("x" << 1), BSON("x" << 2)});
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);

        ASSERT(response.isErrDetailsSet());
        ASSERT_GT(response.sizeErrDetails(), 0u);
        ASSERT_EQ(ErrorCodes::UnknownError, response.getErrDetailsAt(0)->toStatus().code());
    });

    BatchedCommandResponse failedResponse;
    failedResponse.setStatus({ErrorCodes::UnknownError, "dummy error"});

    expectInsertsReturnError({BSON("x" << 1), BSON("x" << 2)}, failedResponse);

    future.default_timed_get();
}

TEST_F(BatchWriteExecTransactionTest, ErrorInBatchSets_WriteError) {
    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        insertOp.setDocuments({BSON("x" << 1), BSON("x" << 2)});
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);

        ASSERT(response.isErrDetailsSet());
        ASSERT_GT(response.sizeErrDetails(), 0u);
        ASSERT_EQ(ErrorCodes::StaleShardVersion, response.getErrDetailsAt(0)->toStatus().code());
    });

    // Any write error works, using SSV for convenience.
    expectInsertsReturnStaleVersionErrors({BSON("x" << 1), BSON("x" << 2)});

    future.default_timed_get();
}

TEST_F(BatchWriteExecTransactionTest, ErrorInBatchSets_WriteErrorOrdered) {
    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase writeCommandBase;
            writeCommandBase.setOrdered(true);
            return writeCommandBase;
        }());
        insertOp.setDocuments({BSON("x" << 1), BSON("x" << 2)});
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);

        ASSERT(response.isErrDetailsSet());
        ASSERT_GT(response.sizeErrDetails(), 0u);
        ASSERT_EQ(ErrorCodes::StaleShardVersion, response.getErrDetailsAt(0)->toStatus().code());
    });

    // Any write error works, using SSV for convenience.
    expectInsertsReturnStaleVersionErrors({BSON("x" << 1), BSON("x" << 2)});

    future.default_timed_get();
}

TEST_F(BatchWriteExecTransactionTest, ErrorInBatchSets_TransientTxnError) {
    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        insertOp.setDocuments({BSON("x" << 1), BSON("x" << 2)});
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        ASSERT_THROWS_CODE(
            BatchWriteExec::executeBatch(
                operationContext(), singleShardNSTargeter, request, &response, &stats),
            AssertionException,
            ErrorCodes::WriteConflict);
    });

    expectInsertsReturnTransientTxnErrors({BSON("x" << 1), BSON("x" << 2)});

    future.default_timed_get();
}

TEST_F(BatchWriteExecTransactionTest, ErrorInBatchSets_DispatchError) {
    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        insertOp.setDocuments({BSON("x" << 1), BSON("x" << 2)});
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;

        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);

        ASSERT(response.isErrDetailsSet());
        ASSERT_GT(response.sizeErrDetails(), 0u);
        ASSERT_EQ(ErrorCodes::CallbackCanceled, response.getErrDetailsAt(0)->toStatus().code());
    });

    onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
        return Status(ErrorCodes::CallbackCanceled, "simulating executor cancel for test");
    });

    future.default_timed_get();
}

TEST_F(BatchWriteExecTransactionTest, ErrorInBatchSets_TransientDispatchError) {
    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        insertOp.setDocuments({BSON("x" << 1), BSON("x" << 2)});
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;

        ASSERT_THROWS_CODE(
            BatchWriteExec::executeBatch(
                operationContext(), singleShardNSTargeter, request, &response, &stats),
            AssertionException,
            ErrorCodes::InterruptedAtShutdown);
    });

    onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
        return Status(ErrorCodes::InterruptedAtShutdown, "simulating shutdown for test");
    });

    future.default_timed_get();
}

}  // namespace
}  // namespace mongo
