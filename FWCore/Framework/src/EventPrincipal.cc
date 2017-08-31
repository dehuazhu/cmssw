#include "FWCore/Framework/interface/EventPrincipal.h"

#include "DataFormats/Common/interface/BasicHandle.h"
#include "DataFormats/Common/interface/FunctorHandleExceptionFactory.h"
#include "DataFormats/Common/interface/ThinnedAssociation.h"
#include "DataFormats/Common/interface/Wrapper.h"
#include "DataFormats/Provenance/interface/BranchIDList.h"
#include "DataFormats/Provenance/interface/BranchIDListHelper.h"
#include "DataFormats/Provenance/interface/BranchListIndex.h"
#include "DataFormats/Provenance/interface/RunLumiEventNumber.h"
#include "DataFormats/Provenance/interface/ProductIDToBranchID.h"
#include "DataFormats/Provenance/interface/ProductRegistry.h"
#include "DataFormats/Provenance/interface/Provenance.h"
#include "DataFormats/Provenance/interface/ThinnedAssociationsHelper.h"
#include "FWCore/Framework/interface/DelayedReader.h"
#include "FWCore/Framework/interface/ProductResolverBase.h"
#include "FWCore/Framework/interface/LuminosityBlockPrincipal.h"
#include "FWCore/Framework/interface/ProductDeletedException.h"
#include "FWCore/Framework/interface/SharedResourcesAcquirer.h"
#include "FWCore/Utilities/interface/Algorithms.h"
#include "FWCore/Utilities/interface/EDMException.h"
#include "FWCore/ServiceRegistry/interface/ModuleCallingContext.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <memory>

namespace edm {
  EventPrincipal::EventPrincipal(
        std::shared_ptr<ProductRegistry const> reg,
        std::shared_ptr<BranchIDListHelper const> branchIDListHelper,
        std::shared_ptr<ThinnedAssociationsHelper const> thinnedAssociationsHelper,
        ProcessConfiguration const& pc,
        HistoryAppender* historyAppender,
        unsigned int streamIndex,
        bool isForPrimaryProcess) :
    Base(reg, reg->productLookup(InEvent), pc, InEvent, historyAppender,isForPrimaryProcess),
          aux_(),
          luminosityBlockPrincipal_(),
          provRetrieverPtr_(new ProductProvenanceRetriever(streamIndex)),
          eventSelectionIDs_(),
          branchIDListHelper_(branchIDListHelper),
          thinnedAssociationsHelper_(thinnedAssociationsHelper),
          branchListIndexes_(),
          branchListIndexToProcessIndex_(),
          streamID_(streamIndex) {
    assert(thinnedAssociationsHelper_);
  }

  void
  EventPrincipal::clearEventPrincipal() {
    clearPrincipal();
    aux_ = EventAuxiliary();
    luminosityBlockPrincipal_ = nullptr; // propagate_const<T> has no reset() function
    provRetrieverPtr_->reset();
    branchListIndexToProcessIndex_.clear();
  }

  void
  EventPrincipal::fillEventPrincipal(EventAuxiliary const& aux,
        ProcessHistoryRegistry const& processHistoryRegistry,
        EventSelectionIDVector&& eventSelectionIDs,
        BranchListIndexes&& branchListIndexes,
        ProductProvenanceRetriever const& provRetriever,
        DelayedReader* reader,
        bool deepCopyRetriever) {
    eventSelectionIDs_ = eventSelectionIDs;
    if (deepCopyRetriever) {
      provRetrieverPtr_->deepCopy(provRetriever);
    } else {
      provRetrieverPtr_->mergeParentProcessRetriever(provRetriever);
    }
    branchListIndexes_ = branchListIndexes;
    if(branchIDListHelper_->hasProducedProducts()) {
      // Add index into BranchIDListRegistry for products produced this process
      branchListIndexes_.push_back(branchIDListHelper_->producedBranchListIndex());
    }
    fillEventPrincipal(aux,processHistoryRegistry,reader);
  }

  void
  EventPrincipal::fillEventPrincipal(EventAuxiliary const& aux,
                                     ProcessHistoryRegistry const& processHistoryRegistry,
                                     EventSelectionIDVector&& eventSelectionIDs,
                                     BranchListIndexes&& branchListIndexes) {
    eventSelectionIDs_ = eventSelectionIDs;
    branchListIndexes_ = branchListIndexes;
    if(branchIDListHelper_->hasProducedProducts()) {
      // Add index into BranchIDListRegistry for products produced this process
      branchListIndexes_.push_back(branchIDListHelper_->producedBranchListIndex());
    }
    fillEventPrincipal(aux,processHistoryRegistry,nullptr);
  }

  void
  EventPrincipal::fillEventPrincipal(EventAuxiliary const& aux,
                                     ProcessHistoryRegistry const& processHistoryRegistry,
                                     DelayedReader* reader) {
    if(aux.event() == invalidEventNumber) {
      throw Exception(errors::LogicError)
        << "EventPrincipal::fillEventPrincipal, Invalid event number provided in EventAuxiliary, It is illegal for the event number to be 0\n";
    }

    fillPrincipal(aux.processHistoryID(), processHistoryRegistry, reader);
    aux_ = aux;
    aux_.setProcessHistoryID(processHistoryID());
    
    if(branchListIndexes_.empty() and branchIDListHelper_->hasProducedProducts()) {
      // Add index into BranchIDListRegistry for products produced this process
      //  if it hasn't already been filled in by the other fillEventPrincipal or by an earlier call to this function
      branchListIndexes_.push_back(branchIDListHelper_->producedBranchListIndex());
    }

    // Fill in helper map for Branch to ProductID mapping
    ProcessIndex pix = 0;
    for(auto const& blindex : branchListIndexes_) {
      branchListIndexToProcessIndex_.insert(std::make_pair(blindex, pix));
      ++pix;
    }

    // Fill in the product ID's in the product holders.
    for(auto& prod : *this) {
      if (prod->singleProduct()) {
        // If an alias is in the same process as the original then isAlias will be true.
        //  Under that condition, we want the ProductID to be the same as the original.
        //  If not, then we've internally changed the original BranchID to the alias BranchID
        //  in the ProductID lookup so we need the alias BranchID.
        auto const & bd =prod->branchDescription();
        prod->setProvenance(productProvenanceRetrieverPtr(),
                            processHistory(),
                            branchIDToProductID(bd.isAlias()?bd.originalBranchID(): bd.branchID()));
      }
    }
  }

  void
  EventPrincipal::setLuminosityBlockPrincipal(std::shared_ptr<LuminosityBlockPrincipal> const& lbp) {
    luminosityBlockPrincipal_ = lbp;
  }

  void 
  EventPrincipal::setRunAndLumiNumber(RunNumber_t run, LuminosityBlockNumber_t lumi) {
    assert(run == luminosityBlockPrincipal_->run());
    assert(lumi == luminosityBlockPrincipal_->luminosityBlock());
    EventNumber_t event = aux_.id().event();
    aux_.id() = EventID(run, lumi, event);
  }

  RunPrincipal const&
  EventPrincipal::runPrincipal() const {
    return luminosityBlockPrincipal().runPrincipal();
  }

  void
  EventPrincipal::put(
        BranchDescription const& bd,
        std::unique_ptr<WrapperBase> edp,
        ProductProvenance const& productProvenance) const {

    // assert commented out for DaqSource.  When DaqSource no longer uses put(), the assert can be restored.
    //assert(produced());
    if(edp.get() == nullptr) {
      throw Exception(errors::InsertFailure, "Null Pointer")
        << "put: Cannot put because ptr to product is null."
        << "\n";
    }
    productProvenanceRetrieverPtr()->insertIntoSet(productProvenance);
    auto phb = getExistingProduct(bd.branchID());
    assert(phb);
    // ProductResolver assumes ownership
    phb->putProduct(std::move(edp));
  }

  void
  EventPrincipal::putOnRead(
        BranchDescription const& bd,
        std::unique_ptr<WrapperBase> edp,
        ProductProvenance const* productProvenance) const {

    assert(!bd.produced());
    if (productProvenance) {
      productProvenanceRetrieverPtr()->insertIntoSet(*productProvenance);
    }
    auto phb = getExistingProduct(bd.branchID());
    assert(phb);
    // ProductResolver assumes ownership
    phb->putProduct(std::move(edp));
  }

  BranchID
  EventPrincipal::pidToBid(ProductID const& pid) const {
    if(!pid.isValid()) {
      throw Exception(errors::ProductNotFound, "InvalidID")
        << "get by product ID: invalid ProductID supplied\n";
    }
    return productIDToBranchID(pid, branchIDListHelper_->branchIDLists(), branchListIndexes_);
  }

  ProductID
  EventPrincipal::branchIDToProductID(BranchID const& bid) const {
    if(!bid.isValid()) {
      throw Exception(errors::NotFound, "InvalidID")
        << "branchIDToProductID: invalid BranchID supplied\n";
    }
    typedef BranchIDListHelper::BranchIDToIndexMap BIDToIndexMap;
    typedef BIDToIndexMap::const_iterator Iter;
    typedef std::pair<Iter, Iter> IndexRange;

    IndexRange range = branchIDListHelper_->branchIDToIndexMap().equal_range(bid);
    for(Iter it = range.first; it != range.second; ++it) {
      BranchListIndex blix = it->second.first;
      std::map<BranchListIndex, ProcessIndex>::const_iterator i = branchListIndexToProcessIndex_.find(blix);
      if(i != branchListIndexToProcessIndex_.end()) {
        ProductIndex productIndex = it->second.second;
        ProcessIndex processIndex = i->second;
        return ProductID(processIndex+1, productIndex+1);
      }
    }
    // cannot throw, because some products may legitimately not have product ID's (e.g. pile-up).
    return ProductID();
  }

  unsigned int
  EventPrincipal::transitionIndex_() const {
    return streamID_.value();
  }

  static void throwProductDeletedException(ProductID const& pid, edm::EventPrincipal::ConstProductResolverPtr const phb) {
    ProductDeletedException exception;
    exception<<"get by product ID: The product with given id: "<<pid
    <<"\ntype: "<<phb->productType()
    <<"\nproduct instance name: "<<phb->productInstanceName()
    <<"\nprocess name: "<<phb->processName()
    <<"\nwas already deleted. This is a configuration error. Please change the configuration of the module which caused this exception to state it reads this data.";
    throw exception;    
  }
  
  BasicHandle
  EventPrincipal::getByProductID(ProductID const& pid) const {
    BranchID bid = pidToBid(pid);
    ConstProductResolverPtr const phb = getProductResolver(bid);
    if(phb == nullptr) {
      return BasicHandle(makeHandleExceptionFactory([pid]()->std::shared_ptr<cms::Exception> {
        std::shared_ptr<cms::Exception> whyFailed(std::make_shared<Exception>(errors::ProductNotFound, "InvalidID"));
        *whyFailed
        << "get by product ID: no product with given id: " << pid << "\n";
        return whyFailed;
      }));
    }

    // Was this already deleted?
    if(phb->productWasDeleted()) {
      throwProductDeletedException(pid, phb);
    }
    // Check for case where we tried on demand production and
    // it failed to produce the object
    if(phb->unscheduledWasNotRun()) {
      return BasicHandle(makeHandleExceptionFactory([pid]()->std::shared_ptr<cms::Exception> {
        std::shared_ptr<cms::Exception> whyFailed(std::make_shared<Exception>(errors::ProductNotFound, "InvalidID"));
        *whyFailed
        << "get by ProductID: could not get product with id: " << pid << "\n"
        << "Unscheduled execution not allowed to get via ProductID.\n";
        return whyFailed;
      }));
    }
    auto resolution = phb->resolveProduct(*this,false,nullptr,nullptr);

    auto data = resolution.data();
    if(data) {
      return BasicHandle(data->wrapper(), &(data->provenance()));
    }
    return BasicHandle(nullptr,nullptr);
  }

  WrapperBase const*
  EventPrincipal::getIt(ProductID const& pid) const {
    return getByProductID(pid).wrapper();
  }

  WrapperBase const*
  EventPrincipal::getThinnedProduct(ProductID const& pid, unsigned int& key) const {

    BranchID parent = pidToBid(pid);

    // Loop over thinned containers which were made by selecting elements from the parent container
    for(auto associatedBranches = thinnedAssociationsHelper_->parentBegin(parent),
                           iEnd = thinnedAssociationsHelper_->parentEnd(parent);
        associatedBranches != iEnd; ++associatedBranches) {

      ThinnedAssociation const* thinnedAssociation =
        getThinnedAssociation(associatedBranches->association());
      if(thinnedAssociation == nullptr) continue;

      if(associatedBranches->parent() != pidToBid(thinnedAssociation->parentCollectionID())) {
        continue;
      }

      unsigned int thinnedIndex = 0;
      // Does this thinned container have the element referenced by key?
      // If yes, thinnedIndex is set to point to it in the thinned container
      if(!thinnedAssociation->hasParentIndex(key, thinnedIndex)) {
        continue;
      }
      // Get the thinned container and return a pointer if we can find it
      ProductID const& thinnedCollectionPID = thinnedAssociation->thinnedCollectionID();
      BasicHandle bhThinned = getByProductID(thinnedCollectionPID);
      if(!bhThinned.isValid()) {
        // Thinned container is not found, try looking recursively in thinned containers
        // which were made by selecting elements from this thinned container.
        WrapperBase const* wrapperBase = getThinnedProduct(thinnedCollectionPID, thinnedIndex);
        if(wrapperBase != nullptr) {
          key = thinnedIndex;
          return wrapperBase;
        } else {
          continue;
        }
      }
      key = thinnedIndex;
      return bhThinned.wrapper();
    }
    return nullptr;
  }

  void
  EventPrincipal::getThinnedProducts(ProductID const& pid,
                                     std::vector<WrapperBase const*>& foundContainers,
                                     std::vector<unsigned int>& keys) const {

    BranchID parent = pidToBid(pid);

    // Loop over thinned containers which were made by selecting elements from the parent container
    for(auto associatedBranches = thinnedAssociationsHelper_->parentBegin(parent),
                           iEnd = thinnedAssociationsHelper_->parentEnd(parent);
        associatedBranches != iEnd; ++associatedBranches) {

      ThinnedAssociation const* thinnedAssociation =
        getThinnedAssociation(associatedBranches->association());
      if(thinnedAssociation == nullptr) continue;

      if(associatedBranches->parent() != pidToBid(thinnedAssociation->parentCollectionID())) {
        continue;
      }

      unsigned nKeys = keys.size();
      unsigned int doNotLookForThisIndex = std::numeric_limits<unsigned int>::max();
      std::vector<unsigned int> thinnedIndexes(nKeys, doNotLookForThisIndex);
      bool hasAny = false;
      for(unsigned k = 0; k < nKeys; ++k) {
        // Already found this one
        if(foundContainers[k] != nullptr) continue;
        // Already know this one is not in this thinned container
        if(keys[k] == doNotLookForThisIndex) continue;
        // Does the thinned container hold the entry of interest?
        // Modifies thinnedIndexes[k] only if it returns true and
        // sets it to the index in the thinned collection.
        if(thinnedAssociation->hasParentIndex(keys[k], thinnedIndexes[k])) {
          hasAny = true;
        }
      }
      if(!hasAny) {
        continue;
      }
      // Get the thinned container and set the pointers and indexes into
      // it (if we can find it)
      ProductID thinnedCollectionPID = thinnedAssociation->thinnedCollectionID();
      BasicHandle bhThinned = getByProductID(thinnedCollectionPID);
      if(!bhThinned.isValid()) {
        // Thinned container is not found, try looking recursively in thinned containers
        // which were made by selecting elements from this thinned container.
        getThinnedProducts(thinnedCollectionPID, foundContainers, thinnedIndexes);
        for(unsigned k = 0; k < nKeys; ++k) {
          if(foundContainers[k] == nullptr) continue;
          if(thinnedIndexes[k] == doNotLookForThisIndex) continue;
          keys[k] = thinnedIndexes[k];
        }
      } else {
        for(unsigned k = 0; k < nKeys; ++k) {
          if(thinnedIndexes[k] == doNotLookForThisIndex) continue;
          keys[k] = thinnedIndexes[k];
          foundContainers[k] = bhThinned.wrapper();
        }
      }
    }
  }

  Provenance
  EventPrincipal::getProvenance(ProductID const& pid, ModuleCallingContext const* mcc) const {
    BranchID bid = pidToBid(pid);
    return getProvenance(bid, mcc);
  }

  EventSelectionIDVector const&
  EventPrincipal::eventSelectionIDs() const {
    return eventSelectionIDs_;
  }

  BranchListIndexes const&
  EventPrincipal::branchListIndexes() const {
    return branchListIndexes_;
  }

  edm::ThinnedAssociation const*
  EventPrincipal::getThinnedAssociation(edm::BranchID const& branchID) const {

    ConstProductResolverPtr const phb = getProductResolver(branchID);

    if(phb == nullptr) {
      throw Exception(errors::LogicError)
        << "EventPrincipal::getThinnedAssociation, ThinnedAssociation ProductResolver cannot be found\n"
        << "This should never happen. Contact a Framework developer";
    }
    ProductData const* productData = (phb->resolveProduct(*this,false,nullptr,nullptr)).data();
    if (productData == nullptr) {
      return nullptr;
    }
    WrapperBase const* product = productData->wrapper();
    if(!(typeid(edm::ThinnedAssociation) == product->dynamicTypeInfo())) {
      throw Exception(errors::LogicError)
        << "EventPrincipal::getThinnedProduct, product has wrong type, not a ThinnedAssociation.\n";
    }
    Wrapper<ThinnedAssociation> const* wrapper = static_cast<Wrapper<ThinnedAssociation> const*>(product);
    return wrapper->product();
  }

}
