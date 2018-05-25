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

#ifndef ARANGOD_GRAPH_GRAPH_H
#define ARANGOD_GRAPH_GRAPH_H

#include <chrono>
#include <utility>

#include "Aql/VariableGenerator.h"
#include "Basics/ReadWriteLock.h"
#include "Cluster/ResultT.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/OperationResult.h"

namespace arangodb {
namespace graph {

class EdgeDefinition {
 public:
  EdgeDefinition(std::string edgeCollection_,
                 std::unordered_set<std::string>&& from_,
                 std::unordered_set<std::string>&& to_)
      : _edgeCollection(std::move(edgeCollection_)), _from(from_), _to(to_) {}

  std::string const& getName() const { return _edgeCollection; }
  std::unordered_set<std::string> const& getFrom() const { return _from; }
  std::unordered_set<std::string> const& getTo() const { return _to; }

  // TODO implement these
  bool isFrom(std::string const& vertexCollection) const;
  bool isTo(std::string const& vertexCollection) const;

 private:
  std::string _edgeCollection;
  std::unordered_set<std::string> _from;
  std::unordered_set<std::string> _to;
};

class Graph {
 public:
  explicit Graph(std::string&& graphName, velocypack::Slice const& info);

  explicit Graph(std::string const& graphName, velocypack::Slice const& info)
      : Graph(std::string{graphName}, info) {}

  virtual ~Graph() = default;

 private:
  /// @brief name of this graph
  std::string const _graphName;

  /// @brief the cids of all vertexCollections
  std::unordered_set<std::string> _vertexColls;

  /// @brief the cids of all edgeCollections
  std::unordered_set<std::string> _edgeColls;

  /// @brief edge definitions of this graph
  std::unordered_map<std::string, EdgeDefinition> _edgeDefs;

  /// @brief Graph collection edge definition attribute name
  static char const* _attrEdgeDefs;

  /// @brief Graph collection orphan list arribute name
  static char const* _attrOrphans;

 public:
  /// @brief Graph collection name
  static std::string const _graphs;

 public:
  /// @brief get the cids of all vertexCollections
  std::unordered_set<std::string> const& vertexCollections() const;

  /// @brief get the cids of all edgeCollections
  std::unordered_set<std::string> const& edgeCollections() const;

  std::string const& name() const { return _graphName; }

  /// @brief return a VelocyPack representation of the graph
  void toVelocyPack(velocypack::Builder&) const;

  virtual void enhanceEngineInfo(velocypack::Builder&) const;

  /// @brief validate the structure of edgeDefinition, i.e.
  /// that it contains the correct attributes, and that they contain the correct
  /// types of values.
  Result validateEdgeDefinition(const velocypack::Slice& edgeDefinition);

  std::ostream& operator<<(std::ostream& ostream);

 private:
  /// @brief adds one edge definition. the edge definition must not have been
  /// added before.
  Result addEdgeDefinition(velocypack::Slice const& edgeDefinition);

  /// @brief Add an edge collection to this graphs definition
  void addEdgeCollection(std::string const&);

  /// @brief Add a vertex collection to this graphs definition
  void addVertexCollection(std::string const&);

  /// @brief Add Collections to the object
  void insertVertexCollections(velocypack::Slice& arr);
};

class GraphOperations {
 private:
  Graph const& _graph;
  std::shared_ptr<transaction::Context> _ctx;

  Graph const& graph() const { return _graph; };
  std::shared_ptr<transaction::Context>& ctx() { return _ctx; };

 public:
  GraphOperations() = delete;
  GraphOperations(Graph const& graph_,
                  std::shared_ptr<transaction::Context> ctx_)
      : _graph(graph_), _ctx(std::move(ctx_)) {}

  // TODO I added the complex result type for the get* methods to exactly
  // reproduce (in the RestGraphHandler) the behaviour of the similar methods
  // in the RestDocumentHandler. A simpler type, e.g. ResultT<OperationResult>,
  // would be preferable.

  /// @brief Get a single vertex document from collection, optionally check rev
  /// The return value is as follows:
  /// If trx.begin fails, the outer ResultT will contain this error Result.
  /// Otherwise, the results of both (trx.document(), trx.finish()) are
  /// returned as a pair.
  /// This is because in case of a precondition error during trx.document(),
  /// the OperationResult may still be needed.
  ResultT<std::pair<OperationResult, Result>> getVertex(
      std::string const& collectionName, std::string const& key,
      boost::optional<TRI_voc_rid_t> rev);

  /// @brief Get a single edge document from definitionName.
  /// Similar to getVertex().
  ResultT<std::pair<OperationResult, Result>> getEdge(
      const std::string& definitionName, const std::string& key,
      boost::optional<TRI_voc_rid_t> rev);

  /// @brief Remove a single edge document from definitionName.
  ResultT<std::pair<OperationResult, Result>> removeEdge(
      const std::string& definitionName, const std::string& key,
      boost::optional<TRI_voc_rid_t> rev, bool waitForSync, bool returnOld);

  /// @brief Remove a vertex and all incident edges in the graph
  ResultT<std::pair<OperationResult, Result>> removeVertex(
      const std::string& collectionName, const std::string& key,
      boost::optional<TRI_voc_rid_t> rev, bool waitForSync, bool returnOld);

  ResultT<std::pair<OperationResult, Result>> updateEdge(
      const std::string& definitionName, const std::string& key,
      VPackSlice document, boost::optional<TRI_voc_rid_t> rev, bool waitForSync,
      bool returnOld, bool returnNew, bool keepNull);

  ResultT<std::pair<OperationResult, Result>> replaceEdge(
      const std::string& definitionName, const std::string& key,
      VPackSlice document, boost::optional<TRI_voc_rid_t> rev, bool waitForSync,
      bool returnOld, bool returnNew, bool keepNull);

  ResultT<std::pair<OperationResult, Result>> createEdge(
      const std::string& definitionName, const std::string& key,
      VPackSlice document, bool waitForSync);

  ResultT<std::pair<OperationResult, Result>> updateVertex(
      const std::string& collectionName, const std::string& key,
      VPackSlice document, boost::optional<TRI_voc_rid_t> rev, bool waitForSync,
      bool returnOld, bool returnNew, bool keepNull);

  ResultT<std::pair<OperationResult, Result>> replaceVertex(
      const std::string& collectionName, const std::string& key,
      VPackSlice document, boost::optional<TRI_voc_rid_t> rev, bool waitForSync,
      bool returnOld, bool returnNew, bool keepNull);

  ResultT<std::pair<OperationResult, Result>> createVertex(
      const std::string& collectionName, const std::string& key,
      VPackSlice document, bool waitForSync);

 private:
  using VPackBufferPtr = std::shared_ptr<velocypack::Buffer<uint8_t>>;

  ResultT<std::pair<OperationResult, Result>> _getDocument(
      const std::string& collectionName, const std::string& key,
      boost::optional<TRI_voc_rid_t> rev);

  VPackBufferPtr _getSearchSlice(const std::string& key,
                                 boost::optional<TRI_voc_rid_t>& rev) const;

  ResultT<std::pair<OperationResult, Result>> _modifyDocument(
      const std::string& collectionName, const std::string& key,
      VPackSlice document, bool isPatch, boost::optional<TRI_voc_rid_t> rev,
      bool waitForSync, bool returnOld, bool returnNew, bool keepNull);

  ResultT<std::pair<OperationResult, Result>> _createDocument(
      const std::string& collectionName, const std::string& key,
      VPackSlice document, bool waitForSync);
};

class GraphCache {
 public:
  // save now() along with the graph
  using EntryType = std::pair<std::chrono::steady_clock::time_point,
                              std::shared_ptr<const Graph>>;
  using CacheType = std::unordered_map<std::string, EntryType>;

  const std::shared_ptr<const Graph> getGraph(
      std::shared_ptr<transaction::Context> ctx, std::string const& name,
      std::chrono::seconds maxAge = std::chrono::seconds(60));

 private:
  basics::ReadWriteLock _lock;
  CacheType _cache;
};
}
}

#endif  // ARANGOD_GRAPH_GRAPH_H
