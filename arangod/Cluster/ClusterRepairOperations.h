////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2018 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Tobias Gödderz
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB3_CLUSTERREPAIROPERATIONS_H
#define ARANGODB3_CLUSTERREPAIROPERATIONS_H

#include <boost/optional.hpp>
#include <boost/variant.hpp>

#include <velocypack/velocypack-common.h>

#include "ClusterInfo.h"

namespace arangodb {
namespace velocypack {
template<typename T>
class Buffer;
}

namespace cluster_repairs {
using DBServers = std::vector<ServerID>;
using VPackBufferPtr = std::shared_ptr<velocypack::Buffer<uint8_t>>;


struct BeginRepairsOperation {
  DatabaseID database;
  CollectionID collectionId;
  std::string collectionName;
  CollectionID protoCollectionId;
  std::string protoCollectionName;
  size_t collectionReplicationFactor;
  size_t protoReplicationFactor;
  bool renameDistributeShardsLike;

  BeginRepairsOperation() = delete;
};

struct FinishRepairsOperation {
  DatabaseID database;
  CollectionID collectionId;
  std::string collectionName;
  CollectionID protoCollectionId;
  std::string protoCollectionName;
  size_t replicationFactor;

  FinishRepairsOperation() = delete;
};

struct MoveShardOperation {
  DatabaseID database;
  CollectionID collectionId;
  std::string collectionName;
  ShardID shard;
  ServerID from;
  ServerID to;
  bool isLeader;

  MoveShardOperation() = delete;

  VPackBufferPtr
  toVpackTodo(
    uint64_t jobId,
    boost::posix_time::ptime jobCreationTimestamp
  ) const;
};

struct FixServerOrderOperation {
  DatabaseID database;
  CollectionID collectionId;
  std::string collectionName;
  CollectionID protoCollectionId;
  std::string protoCollectionName;
  ShardID shard;
  ShardID protoShard;
  ServerID leader;
  std::vector<ServerID> followers;
  std::vector<ServerID> protoFollowers;

  FixServerOrderOperation() = delete;
};


bool operator==(BeginRepairsOperation const &left, BeginRepairsOperation const &right);

bool operator==(FinishRepairsOperation const &left, FinishRepairsOperation const &right);

bool operator==(MoveShardOperation const &left, MoveShardOperation const &right);

bool operator==(FixServerOrderOperation const &left, FixServerOrderOperation const &right);

std::ostream &operator<<(std::ostream &ostream, BeginRepairsOperation const &operation);

std::ostream &operator<<(std::ostream &ostream, FinishRepairsOperation const &operation);

std::ostream &operator<<(std::ostream &ostream, MoveShardOperation const &operation);

std::ostream &operator<<(std::ostream &ostream, FixServerOrderOperation const &operation);

using RepairOperation = boost::variant<
  BeginRepairsOperation const,
  FinishRepairsOperation const,
  MoveShardOperation const,
  FixServerOrderOperation const
>;

std::ostream &operator<<(std::ostream &ostream, RepairOperation const &operation);

// Converts any RepairOperation to a Transaction. If its a job (i.e. put in
// Target/ToDo/), it returns the corresponding job id as well.
class RepairOperationToTransactionVisitor
  : public boost::static_visitor<
    std::pair<AgencyWriteTransaction, boost::optional<uint64_t>>
  > {
  using ReturnValueT = std::pair<AgencyWriteTransaction, boost::optional<uint64_t>>;

 public:
  RepairOperationToTransactionVisitor();
  RepairOperationToTransactionVisitor(
    std::function<uint64_t()> getJobId,
    std::function<boost::posix_time::ptime()> getJobCreationTimestamp
  );

  ReturnValueT
  operator()(BeginRepairsOperation const &op);

  ReturnValueT
  operator()(FinishRepairsOperation const &op);

  ReturnValueT
  operator()(MoveShardOperation const &op);

  ReturnValueT
  operator()(FixServerOrderOperation const &op);


 private:
  std::vector<VPackBufferPtr> _vpackBufferArray;
  std::function<uint64_t()> _getJobId;
  std::function<boost::posix_time::ptime()> _getJobCreationTimestamp;


  std::vector<VPackBufferPtr>&& steal();


  std::string agencyCollectionId(
    DatabaseID database,
    CollectionID collection
  ) const;


  VPackBufferPtr
  createShardDbServerArray(
    ServerID const &leader,
    DBServers const &followers
  ) const;

};

}
}

#endif //ARANGODB3_CLUSTERREPAIROPERATIONS_H
