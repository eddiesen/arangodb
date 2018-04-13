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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "DocumentProducingBlock.h"
#include "Aql/AqlItemBlock.h"
#include "Aql/DocumentProducingNode.h"
#include "Aql/ExecutionNode.h"
#include "Aql/IndexNode.h"
#include "Aql/Variable.h"
#include "Basics/Exceptions.h"
#include "Basics/StaticStrings.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/StorageEngine.h"
#include "Transaction/Helpers.h"
      
using namespace arangodb;
using namespace arangodb::aql;

DocumentProducingBlock::DocumentProducingBlock(DocumentProducingNode const* node, transaction::Methods* trx)
    : _trxPtr(trx),
      _node(node),
      _produceResult(dynamic_cast<ExecutionNode const*>(_node)->isVarUsedLater(_node->outVariable())),
      _useRawDocumentPointers(EngineSelectorFeature::ENGINE->useRawDocumentPointers()),
      _documentProducer(buildCallback()) {
}

DocumentProducingBlock::DocumentProducingFunction DocumentProducingBlock::buildCallback() const {
  if (!_produceResult) {
    // no result needed
    return [](AqlItemBlock* res, VPackSlice, size_t registerId, size_t& row,
              size_t fromRow) {
      if (row != fromRow) {
        // re-use already copied AQLValues
        res->copyValuesFromRow(row, static_cast<RegisterId>(registerId), fromRow);
      }
      ++row;
    };
  }

  if (!_node->projections().empty()) {
    // return a projection
    return [this](AqlItemBlock* res, VPackSlice slice, size_t registerId, size_t& row, size_t fromRow) {
      VPackBuilder b;
      b.openObject();
      for (auto const& it : _node->projections()) {
        if (it == StaticStrings::IdString) {
          VPackSlice found = transaction::helpers::extractIdFromDocument(slice);
          if (found.isCustom()) {
            // _id as a custom type needs special treatment
            b.add(it, VPackValue(transaction::helpers::extractIdString(_trxPtr->resolver(), found, slice)));
          } else {
            b.add(it, found);
          }
        } else if (it == StaticStrings::KeyString) {
          VPackSlice found = transaction::helpers::extractKeyFromDocument(slice);
          if (_useRawDocumentPointers) {
            b.add(VPackValue(it));
            b.addExternal(found.begin());
          } else {
            b.add(it, found);
          }
        } else {
          slice = slice.get(it);
          if (slice.isNone()) {
            // attribute not found
            b.add(it, VPackValue(VPackValueType::Null));
          } else {
            uint8_t const* found = slice.begin();
            if (_useRawDocumentPointers) {
              b.add(VPackValue(it));
              b.addExternal(found);
            } else {
              b.add(it, VPackSlice(found));
            }
          }
        }
      }
      b.close();
            
      res->emplaceValue(row, static_cast<arangodb::aql::RegisterId>(registerId), AqlValue(b));
      
      if (row != fromRow) {
        // re-use already copied AQLValues
        res->copyValuesFromRow(row, static_cast<RegisterId>(registerId), fromRow);
      }
      ++row;

    };
  }

  // return the document as is
  return [this](AqlItemBlock* res, VPackSlice slice, size_t registerId, size_t& row, size_t fromRow) {
    uint8_t const* vpack = slice.begin();
    if (_useRawDocumentPointers) {
      res->emplaceValue(row, static_cast<arangodb::aql::RegisterId>(registerId),
                        AqlValueHintDocumentNoCopy(vpack));
    } else {
      res->emplaceValue(row, static_cast<arangodb::aql::RegisterId>(registerId),
                        AqlValueHintCopy(vpack));
    }
    if (row != fromRow) {
      // re-use already copied AQLValues
      res->copyValuesFromRow(row, static_cast<RegisterId>(registerId), fromRow);
    }
    ++row;
  };
}
