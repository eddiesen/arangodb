////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
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
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#include "MMFilesGeoS2Index.h"

#include "Aql/Ast.h"
#include "Aql/AstNode.h"
#include "Aql/SortCondition.h"
#include "Basics/StringRef.h"
#include "Basics/VelocyPackHelper.h"
#include "Geo/GeoUtils.h"
#include "GeoIndex/Near.h"
#include "Indexes/IndexIterator.h"
#include "Indexes/IndexResult.h"
#include "Logger/Logger.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/ManagedDocumentResult.h"

#include <velocypack/Iterator.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;

/*struct SimpleIterator final : public IndexIterator {
  char const* typeName() const override { return "s2-index-iterator"; }

  /// @brief Construct an RocksDBGeoIndexIterator based on Ast Conditions
  SimpleIterator(LogicalCollection* collection, transaction::Methods* trx,
                 ManagedDocumentResult* mmdr, MMFilesGeoS2Index const* index,
                 geo::QueryParams&& params)
  : IndexIterator(collection, trx, mmdr, index),
  _index(index),
  _params(std::move(params)) {

  }

  /// internal retrieval loop
  inline bool nextToken(std::function<bool(LocalDocumentId token)>&& cb,
                        size_t limit) {
    if (_iter == _intervals.end()) {
      // we already know that no further results will be returned by the index
      return false;
    }

    while (limit > 0 && _iter != _intervals.end()) {
      TRI_ASSERT(<#expr#>)
      /while (limit > 0 && _near.hasNearest()) {
        if (cb(_near.nearest().document)) {
          limit--;
        }
        _near.popNearest();
      }
      // need to fetch more geo results
      if (limit > 0 && !_near.isDone()) {
        TRI_ASSERT(!_near.hasNearest());
        performScan();
      }*
      _iter++;
    }
    return _iter != _intervals.end();
  }


  void reset() override {  }

private:
  MMFilesGeoS2Index const* _index;
  geo::QueryParams const _params;
  std::vector<geo::Interval> _intervals;
  std::vector<geo::Interval>::const_iterator _iter;
  std::unordered_set<LocalDocumentId> _seen;
};*/

template <typename CMP = geo_index::DocumentsAscending>
struct NearIterator final : public IndexIterator {
  /// @brief Construct an RocksDBGeoIndexIterator based on Ast Conditions
  NearIterator(LogicalCollection* collection, transaction::Methods* trx,
               ManagedDocumentResult* mmdr, MMFilesGeoS2Index const* index,
               geo::QueryParams&& params)
      : IndexIterator(collection, trx, mmdr, index),
        _index(index),
        _near(std::move(params),
              index->variant() == geo_index::Index::Variant::GEOJSON),
        _scans{0} {
    estimateDensity();
  }

  ~NearIterator() {
    LOG_TOPIC(DEBUG, Logger::ROCKSDB)
        << "near iterator performed " << _scans << " scans";
  }

  char const* typeName() const override { return "s2-index-iterator"; }

  /// internal retrieval loop
  inline bool nextToken(std::function<bool(LocalDocumentId token)>&& cb,
                        size_t limit) {
    if (_near.isDone()) {
      // we already know that no further results will be returned by the index
      TRI_ASSERT(!_near.hasNearest());
      return false;
    }

    while (limit > 0 && !_near.isDone()) {
      while (limit > 0 && _near.hasNearest()) {
        if (cb(_near.nearest().document)) {
          limit--;
        }
        _near.popNearest();
      }
      // need to fetch more geo results
      if (limit > 0 && !_near.isDone()) {
        TRI_ASSERT(!_near.hasNearest());
        performScan();
      }
    }
    return !_near.isDone();
  }

  inline bool nextTokenWithDistance(
      std::function<bool(LocalDocumentId token, double distRad)>&& cb,
      size_t limit) {
    if (_near.isDone()) {
      // we already know that no further results will be returned by the index
      TRI_ASSERT(!_near.hasNearest());
      return false;
    }

    while (limit > 0 && !_near.isDone()) {
      while (limit > 0 && _near.hasNearest()) {
        auto nearest = _near.nearest();
        if (cb(nearest.document, nearest.distRad)) {
          limit--;
        }
        _near.popNearest();
      }
      // need to fetch more geo results
      if (limit > 0 && !_near.isDone()) {
        TRI_ASSERT(!_near.hasNearest());
        performScan();
      }
    }
    return !_near.isDone();
  }

  bool nextDocument(DocumentCallback const& cb, size_t limit) override {
    return nextToken(
        [this, &cb](LocalDocumentId const& token) -> bool {
          if (!_collection->readDocument(_trx, token, *_mmdr)) {
            return false;  // skip
          }
          VPackSlice doc(_mmdr->vpack());
          geo::FilterType const ft = _near.filterType();
          if (ft != geo::FilterType::NONE) {  // expensive test
            geo::ShapeContainer const& filter = _near.filterShape();
            TRI_ASSERT(!filter.empty());
            geo::ShapeContainer test;
            Result res = _index->shape(doc, test);
            TRI_ASSERT(res.ok() &&
                       !test.empty());  // this should never fail here
            if (res.fail() ||
                (ft == geo::FilterType::CONTAINS && !filter.contains(&test)) ||
                (ft == geo::FilterType::INTERSECTS &&
                 !filter.intersects(&test))) {
              return false;  // skip
            }
          }
          cb(token, doc);  // return result
          return true;
        },
        limit);
  }

  bool next(LocalDocumentIdCallback const& cb, size_t limit) override {
    return nextToken(
        [this, &cb](LocalDocumentId const& token) -> bool {
          geo::FilterType const ft = _near.filterType();
          if (ft != geo::FilterType::NONE) {
            geo::ShapeContainer const& filter = _near.filterShape();
            TRI_ASSERT(!filter.empty());
            if (!_collection->readDocument(_trx, token, *_mmdr)) {
              return false;
            }
            geo::ShapeContainer test;
            Result res = _index->shape(VPackSlice(_mmdr->vpack()), test);
            TRI_ASSERT(res.ok());  // this should never fail here
            if (res.fail() ||
                (ft == geo::FilterType::CONTAINS && !filter.contains(&test)) ||
                (ft == geo::FilterType::INTERSECTS &&
                 !filter.intersects(&test))) {
              return false;
            }
          }
          cb(token);  // return result
          return true;
        },
        limit);
  }

  void reset() override { _near.reset(); }

 private:
  size_t _scans;
  // we need to get intervals representing areas in a ring (annulus)
  // around our target point. We need to fetch them ALL and then sort
  // found results in a priority list according to their distance
  void performScan() {
    MMFilesGeoS2Index::IndexTree const& tree = _index->tree();

    // list of sorted intervals to scan
    std::vector<geo::Interval> const scan = _near.intervals();
    if (!_near.isDone()) {
      // TODO TRI_ASSERT(scan.size() > 0);
      ++_scans;
    };
    // LOG_TOPIC(INFO, Logger::FIXME) << "# intervals: " << scan.size();
    // size_t seeks = 0;

    auto it = tree.begin();
    for (size_t i = 0; i < scan.size(); i++) {
      geo::Interval const& interval = scan[i];
      TRI_ASSERT(interval.min <= interval.max);

      // intervals are sorted and likely consecutive, try to avoid seeks
      // by checking whether we are in the range already
      bool seek = true;
      if (i > 0) {
        TRI_ASSERT(scan[i - 1].max < interval.min);
        if (it == tree.end()) {  // no more valid keys after this
          break;
        } else if (it->first > interval.max) {
          continue;  // beyond range already
        } else if (interval.min <= it->first) {
          seek = false;  // already in range: min <= key <= max
          TRI_ASSERT(it->first <= interval.max);
        }
      }

      if (seek) {  // try to avoid seeking at all cost
        it = tree.lower_bound(interval.min);  // seeks++;
      }

      while (it != tree.end() && it->first <= interval.max) {
        _near.reportFound(it->second.documentId, it->second.centroid);
        it++;
      }
    }
    // LOG_TOPIC(INFO, Logger::FIXME) << "# seeks: " << seeks;
  }

  /// find the first indexed entry to estimate the # of entries
  /// around our target coordinates
  void estimateDensity() {
    MMFilesGeoS2Index::IndexTree const& tree = _index->tree();
    if (!tree.empty()) {
      S2CellId cell = S2CellId(_near.origin());
      auto it = tree.upper_bound(cell);
      if (it == tree.end()) {
        it = tree.lower_bound(cell);
      }
      if (it != tree.end()) {
        _near.estimateDensity(it->second.centroid);
      }
    }
  }

 private:
  MMFilesGeoS2Index const* _index;
  geo_index::NearUtils<CMP> _near;
};

MMFilesGeoS2Index::MMFilesGeoS2Index(TRI_idx_iid_t iid,
                                     LogicalCollection* collection,
                                     VPackSlice const& info,
                                     std::string const& typeName)
    : MMFilesIndex(iid, collection, info),
      geo_index::Index(info, _fields),
      _typeName(typeName) {
  TRI_ASSERT(iid != 0);
  _unique = false;
  _sparse = true;
  TRI_ASSERT(_variant != geo_index::Index::Variant::NONE);
}

size_t MMFilesGeoS2Index::memory() const { return _tree.bytes_used(); }

/// @brief return a JSON representation of the index
void MMFilesGeoS2Index::toVelocyPack(VPackBuilder& builder, bool withFigures,
                                     bool forPersistence) const {
  TRI_ASSERT(_variant != geo_index::Index::Variant::NONE);
  builder.openObject();
  // Basic index
  // RocksDBIndex::toVelocyPack(builder, withFigures, forPersistence);
  MMFilesIndex::toVelocyPack(builder, withFigures, forPersistence);
  _coverParams.toVelocyPack(builder);
  builder.add("geoJson",
              VPackValue(_variant == geo_index::Index::Variant::GEOJSON));
  // geo indexes are always non-unique
  builder.add("unique", VPackValue(false));
  // geo indexes are always sparse.
  builder.add("sparse", VPackValue(true));
  if (_typeName == "geo1" || _typeName == "geo2") {
    // flags for backwards compatibility
    builder.add("ignoreNull", VPackValue(true));
    builder.add("constraint", VPackValue(false));
  }
  builder.close();
}

/// @brief Test if this index matches the definition
bool MMFilesGeoS2Index::matchesDefinition(VPackSlice const& info) const {
  TRI_ASSERT(_variant != geo_index::Index::Variant::NONE);
  TRI_ASSERT(info.isObject());
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  VPackSlice typeSlice = info.get("type");
  TRI_ASSERT(typeSlice.isString());
  StringRef typeStr(typeSlice);
  TRI_ASSERT(typeStr == oldtypeName());
#endif
  auto value = info.get("id");
  if (!value.isNone()) {
    // We already have an id.
    if (!value.isString()) {
      // Invalid ID
      return false;
    }
    // Short circuit. If id is correct the index is identical.
    StringRef idRef(value);
    return idRef == std::to_string(_iid);
  }

  if (_unique !=
      basics::VelocyPackHelper::getBooleanValue(info, "unique", false)) {
    return false;
  }
  if (_sparse !=
      basics::VelocyPackHelper::getBooleanValue(info, "sparse", true)) {
    return false;
  }

  value = info.get("fields");
  if (!value.isArray()) {
    return false;
  }

  size_t const n = static_cast<size_t>(value.length());
  if (n != _fields.size()) {
    return false;
  }

  if (n == 1) {
    bool geoJson1 =
        basics::VelocyPackHelper::getBooleanValue(info, "geoJson", false);
    bool geoJson2 = _variant == geo_index::Index::Variant::GEOJSON;
    if (geoJson1 != geoJson2) {
      return false;
    }
  }

  // This check takes ordering of attributes into account.
  std::vector<arangodb::basics::AttributeName> translate;
  for (size_t i = 0; i < n; ++i) {
    translate.clear();
    VPackSlice f = value.at(i);
    if (!f.isString()) {
      // Invalid field definition!
      return false;
    }
    arangodb::StringRef in(f);
    TRI_ParseAttributeString(in, translate, true);
    if (!basics::AttributeName::isIdentical(_fields[i], translate, false)) {
      return false;
    }
  }
  return true;
}

Result MMFilesGeoS2Index::insert(transaction::Methods*,
                                 LocalDocumentId const& documentId,
                                 VPackSlice const& doc, OperationMode mode) {
  // covering and centroid of coordinate / polygon / ...
  std::vector<S2CellId> cells;
  geo::Coordinate centroid(-1.0, -1.0);
  Result res = geo_index::Index::indexCells(doc, cells, centroid);
  if (res.fail()) {
    // Invalid, no insert. Index is sparse
    return res.is(TRI_ERROR_BAD_PARAMETER) ? IndexResult() : res;
  }
  // LOG_TOPIC(ERR, Logger::FIXME) << "Inserting #cells " << cells.size() << "
  // doc: " << doc.toJson() << " center: " << centroid.toString();
  TRI_ASSERT(!cells.empty() && std::abs(centroid.latitude) <= 90.0 &&
             std::abs(centroid.longitude) <= 180.0);
  IndexValue value(documentId, std::move(centroid));

  for (S2CellId cell : cells) {
    _tree.insert(std::make_pair(cell, value));
  }

  return IndexResult();
}

Result MMFilesGeoS2Index::remove(transaction::Methods*,
                                 LocalDocumentId const& documentId,
                                 VPackSlice const& doc, OperationMode mode) {
  // covering and centroid of coordinate / polygon / ...
  std::vector<S2CellId> cells;
  geo::Coordinate centroid(-1, -1);

  Result res = geo_index::Index::indexCells(doc, cells, centroid);
  if (res.fail()) {  // might occur if insert is rolled back
    // Invalid, no insert. Index is sparse
    return res.is(TRI_ERROR_BAD_PARAMETER) ? IndexResult() : res;
  }
  // LOG_TOPIC(ERR, Logger::FIXME) << "Removing #cells " << cells.size() << "
  // doc: " << doc.toJson();
  TRI_ASSERT(!cells.empty() && std::abs(centroid.latitude) <= 90.0 &&
             std::abs(centroid.longitude) <= 180.0);

  for (S2CellId cell : cells) {
    for (auto it = _tree.lower_bound(cell);
         it != _tree.end() && it->first == cell;) {
      if (it->second.documentId == documentId) {
        it = _tree.erase(it);
      } else {
        ++it;
      }
    }
  }
  return IndexResult();
}

/// @brief creates an IndexIterator for the given Condition
IndexIterator* MMFilesGeoS2Index::iteratorForCondition(
    transaction::Methods* trx, ManagedDocumentResult* mmdr,
    arangodb::aql::AstNode const* node,
    arangodb::aql::Variable const* reference,
    IndexIteratorOptions const& opts) {
  TRI_ASSERT(!isSorted() || opts.sorted);
  TRI_ASSERT(!opts.evaluateFCalls);  // should not get here without
  TRI_ASSERT(node != nullptr);

  geo::QueryParams params;
  params.sorted = opts.sorted;
  params.ascending = opts.ascending;
  params.fullRange = opts.fullRange;
  params.limit = opts.limit;
  geo_index::Index::parseCondition(node, reference, params);

  // FIXME: <Optimize away>
  params.sorted = true;
  if (params.filterType != geo::FilterType::NONE) {
    TRI_ASSERT(!params.filterShape.empty());
    params.filterShape.updateBounds(params);
  }
  //        </Optimize away>

  TRI_ASSERT(!opts.sorted || params.origin.isValid());
  // params.cover.worstIndexedLevel < _coverParams.worstIndexedLevel
  // is not necessary, > would be missing entries.
  params.cover.worstIndexedLevel = _coverParams.worstIndexedLevel;
  if (params.cover.bestIndexedLevel > _coverParams.bestIndexedLevel) {
    // it is unnessesary to use a better level than configured
    params.cover.bestIndexedLevel = _coverParams.bestIndexedLevel;
  }
  if (params.ascending) {
    return new NearIterator<geo_index::DocumentsAscending>(
        _collection, trx, mmdr, this, std::move(params));
  } else {
    return new NearIterator<geo_index::DocumentsDescending>(
        _collection, trx, mmdr, this, std::move(params));
  }
}

void MMFilesGeoS2Index::unload() {
  _tree.clear();  // TODO: do we need load?
}

namespace {
void retrieveNear(MMFilesGeoS2Index const& index, transaction::Methods* trx,
                  double lat, double lon, double radius, size_t count,
                  std::string const& attributeName, VPackBuilder& builder) {
  geo::QueryParams params;
  params.pointsOnly = index.pointsOnly();
  params.origin = {lat, lon};
  params.sorted = true;
  if (radius > 0.0) {
    params.maxDistance = radius;
    params.fullRange = true;
  }
  params.limit = count;
  size_t limit = (count > 0) ? count : SIZE_MAX;

  ManagedDocumentResult mmdr;
  LogicalCollection* collection = index.collection();
  NearIterator<geo_index::DocumentsAscending> iter(collection, trx, &mmdr,
                                                   &index, std::move(params));
  auto fetchDoc = [&](LocalDocumentId const& token, double distRad) -> bool {
    bool read = collection->readDocument(trx, token, mmdr);
    if (!read) {
      return false;
    }
    VPackSlice doc(mmdr.vpack());
    double distance = distRad * geo::kEarthRadiusInMeters;

    // add to builder results
    if (!attributeName.empty()) {
      // We have to copy the entire document
      VPackObjectBuilder docGuard(&builder);
      builder.add(attributeName, VPackValue(distance));
      for (auto const& entry : VPackObjectIterator(doc)) {
        std::string key = entry.key.copyString();
        if (key != attributeName) {
          builder.add(key, entry.value);
        }
      }
    } else {
      mmdr.addToBuilder(builder, true);
    }

    return true;
  };

  bool more = iter.nextTokenWithDistance(fetchDoc, limit);
  TRI_ASSERT(count > 0 || !more);
}
}  // namespace

/// @brief looks up all points within a given radius
void MMFilesGeoS2Index::withinQuery(transaction::Methods* trx, double lat,
                                    double lon, double radius,
                                    std::string const& attributeName,
                                    VPackBuilder& builder) const {
  ::retrieveNear(*this, trx, lat, lon, radius, 0, attributeName, builder);
}

void MMFilesGeoS2Index::nearQuery(transaction::Methods* trx, double lat,
                                  double lon, size_t count,
                                  std::string const& attributeName,
                                  VPackBuilder& builder) const {
  ::retrieveNear(*this, trx, lat, lon, -1.0, count, attributeName, builder);
}
