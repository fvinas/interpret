// Copyright (c) 2018 Microsoft Corporation
// Licensed under the MIT license.
// Author: Paul Koch <code@koch.ninja>

#include "precompiled_header_cpp.hpp"

#include <stddef.h> // size_t, ptrdiff_t

#include "ebm_native.h"
#include "logging.h"
#include "zones.h"

#include "ebm_internal.hpp"

#include "ebm_stats.hpp"

#include "Feature.hpp"
#include "FeatureGroup.hpp"
#include "DataSetInteraction.hpp"

#include "InteractionCore.hpp"
#include "InteractionShell.hpp"

#include "HistogramTargetEntry.hpp"
#include "HistogramBucket.hpp"

namespace DEFINED_ZONE_NAME {
#ifndef DEFINED_ZONE_NAME
#error DEFINED_ZONE_NAME must be defined
#endif // DEFINED_ZONE_NAME

template<ptrdiff_t cCompilerClasses, size_t cCompilerDimensions>
class BinSumsInteractionInternal final {
public:

   BinSumsInteractionInternal() = delete; // this is a static class.  Do not construct

   static void Func(InteractionShell * const pInteractionShell, const Term * const pTerm) {
      constexpr bool bClassification = IsClassification(cCompilerClasses);

      LOG_0(TraceLevelVerbose, "Entered BinSumsInteractionInternal");

      BinBase * const aBinsBase = pInteractionShell->GetBinBaseFast();
      auto * const aBins = aBinsBase->Specialize<FloatFast, bClassification>();

      InteractionCore * const pInteractionCore = pInteractionShell->GetInteractionCore();
      const ptrdiff_t cRuntimeClasses = pInteractionCore->GetCountClasses();

      const ptrdiff_t cClasses = GET_COUNT_CLASSES(
         cCompilerClasses,
         cRuntimeClasses
      );
      const size_t cScores = GetCountScores(cClasses);
      EBM_ASSERT(!IsOverflowBinSize<FloatFast>(bClassification, cScores)); // we're accessing allocated memory
      const size_t cBytesPerBin = GetBinSize<FloatFast>(bClassification, cScores);

      const DataSetInteraction * const pDataSet = pInteractionCore->GetDataSetInteraction();
      const FloatFast * pGradientAndHessian = pDataSet->GetGradientsAndHessiansPointer();
      const FloatFast * const pGradientsAndHessiansEnd = pGradientAndHessian + (bClassification ? 2 : 1) * cScores * pDataSet->GetCountSamples();

      const FloatFast * pWeight = pDataSet->GetWeights();

      EBM_ASSERT(pTerm->GetCountDimensions() == pTerm->GetCountSignificantDimensions()); // for interactions, we just return 0 for interactions with zero features
      const size_t cDimensions = GET_DIMENSIONS(cCompilerDimensions, pTerm->GetCountSignificantDimensions());
      EBM_ASSERT(1 <= cDimensions); // for interactions, we just return 0 for interactions with zero features

#ifndef NDEBUG
      FloatFast weightTotalDebug = 0;
#endif // NDEBUG

      for(size_t iSample = 0; pGradientsAndHessiansEnd != pGradientAndHessian; ++iSample) {
         // this loop gets about twice as slow if you add a single unpredictable branching if statement based on count, even if you still access all the memory
         // in complete sequential order, so we'll probably want to use non-branching instructions for any solution like conditional selection or multiplication
         // this loop gets about 3 times slower if you use a bad pseudo random number generator like rand(), although it might be better if you inlined rand().
         // this loop gets about 10 times slower if you use a proper pseudo random number generator like std::default_random_engine
         // taking all the above together, it seems unlikley we'll use a method of separating sets via single pass randomized set splitting.  Even if count is 
         // stored in memory if shouldn't increase the time spent fetching it by 2 times, unless our bottleneck when threading is overwhelmingly memory pressure 
         // related, and even then we could store the count for a single bit aleviating the memory pressure greatly, if we use the right sampling method 

         // TODO : try using a sampling method with non-repeating samples, and put the count into a bit.  Then unwind that loop either at the byte level 
         //   (8 times) or the uint64_t level.  This can be done without branching and doesn't require random number generators

         // TODO : we can elminate the inner vector loop for regression at least, and also if we add a templated bool for binary class.  Propegate this change 
         //   to all places that we loop on the vector

         size_t cTensorBins = 1;
         size_t iTensorBin = 0;
         size_t iDimension = 0;
         do {
            const Feature * const pInputFeature = pTerm->GetTermEntries()[iDimension].m_pFeature;
            const size_t cBins = pInputFeature->GetCountBins();
            // interactions return interaction score of zero earlier on any useless dimensions
            // we strip dimensions from the tensors with 1 bin, so if 1 bin was accepted here, we'd need to strip
            // the bin too
            EBM_ASSERT(size_t { 2 } <= cBins);
            const StorageDataType * pInputData = pDataSet->GetInputDataPointer(pInputFeature);
            pInputData += iSample;
            StorageDataType iBinOriginal = *pInputData;
            EBM_ASSERT(!IsConvertError<size_t>(iBinOriginal));
            size_t iBin = static_cast<size_t>(iBinOriginal);
            EBM_ASSERT(iBin < cBins);
            iTensorBin += cTensorBins * iBin;
            cTensorBins *= cBins;
            ++iDimension;
         } while(iDimension < cDimensions);

         auto * pBin = 
            IndexBin(cBytesPerBin, aBins, iTensorBin);
         ASSERT_BIN_OK(cBytesPerBin, pBin, pInteractionShell->GetBinsFastEndDebug());
         pBin->SetCountSamples(pBin->GetCountSamples() + 1);
         FloatFast weight = 1;
         if(nullptr != pWeight) {
            weight = *pWeight;
            ++pWeight;
#ifndef NDEBUG
            weightTotalDebug += weight;
#endif // NDEBUG
         }
         pBin->SetWeight(pBin->GetWeight() + weight);

         auto * const pGradientPair = pBin->GetGradientPairs();

         for(size_t iScore = 0; iScore < cScores; ++iScore) {
            const FloatFast gradient = *pGradientAndHessian;
            // gradient could be NaN
            // for classification, gradient can be anything from -1 to +1 (it cannot be infinity!)
            // for regression, gradient can be anything from +infinity or -infinity
            pGradientPair[iScore].m_sumGradients += gradient * weight;
            // m_sumGradients could be NaN, or anything from +infinity or -infinity in the case of regression
            if(bClassification) {
               EBM_ASSERT(
                  std::isnan(gradient) ||
                  !std::isinf(gradient) && 
                  -1 - k_epsilonGradient <= gradient && gradient <= 1
                  );

               // TODO : this code gets executed for each SamplingSet set.  I could probably execute it once and then all the SamplingSet
               //   sets would have this value, but I would need to store the computation in a new memory place, and it might make more sense to calculate this 
               //   values in the CPU rather than put more pressure on memory.  I think controlling this should be done in a MACRO and we should use a class to 
               //   hold the gradient and this computation from that value and then comment out the computation if not necssary and access it through an 
               //   accessor so that we can make the change entirely via macro
               const FloatFast hessian = *(pGradientAndHessian + 1);
               EBM_ASSERT(
                  std::isnan(hessian) ||
                  !std::isinf(hessian) && -k_epsilonGradient <= hessian && hessian <= FloatFast { 0.25 }
               ); // since any one hessian is limited to 0 <= hessian <= 0.25, the sum must be representable by a 64 bit number, 

               const FloatFast oldHessian = pGradientPair[iScore].GetSumHessians();
               // since any one hessian is limited to 0 <= gradient <= 0.25, the sum must be representable by a 64 bit number, 
               EBM_ASSERT(std::isnan(oldHessian) || !std::isinf(oldHessian) && -k_epsilonGradient <= oldHessian);
               const FloatFast newHessian = oldHessian + hessian * weight;
               // since any one hessian is limited to 0 <= hessian <= 0.25, the sum must be representable by a 64 bit number, 
               EBM_ASSERT(std::isnan(newHessian) || !std::isinf(newHessian) && -k_epsilonGradient <= newHessian);
               // which will always be representable by a float or double, so we can't overflow to inifinity or -infinity
               pGradientPair[iScore].SetSumHessians(newHessian);
            }
            pGradientAndHessian += bClassification ? 2 : 1;
         }
      }
      EBM_ASSERT(0 < pDataSet->GetWeightTotal());
      EBM_ASSERT(nullptr == pWeight || static_cast<FloatBig>(weightTotalDebug * 0.999) <= pDataSet->GetWeightTotal() && 
         pDataSet->GetWeightTotal() <= static_cast<FloatBig>(1.001 * weightTotalDebug));
      EBM_ASSERT(nullptr != pWeight || 
         static_cast<FloatBig>(pDataSet->GetCountSamples()) == pDataSet->GetWeightTotal());

      LOG_0(TraceLevelVerbose, "Exited BinSumsInteractionInternal");
   }
};

template<ptrdiff_t cCompilerClasses, size_t cCompilerDimensionsPossible>
class BinSumsInteractionDimensions final {
public:

   BinSumsInteractionDimensions() = delete; // this is a static class.  Do not construct

   INLINE_ALWAYS static void Func(InteractionShell * const pInteractionShell, const Term * const pTerm) {
      static_assert(1 <= cCompilerDimensionsPossible, "can't have less than 1 dimension for interactions");
      static_assert(cCompilerDimensionsPossible <= k_cDimensionsMax, "can't have more than the max dimensions");

      const size_t cRuntimeDimensions = pTerm->GetCountSignificantDimensions();

      EBM_ASSERT(1 <= cRuntimeDimensions);
      EBM_ASSERT(cRuntimeDimensions <= k_cDimensionsMax);
      if(cCompilerDimensionsPossible == cRuntimeDimensions) {
         BinSumsInteractionInternal<cCompilerClasses, cCompilerDimensionsPossible>::Func(pInteractionShell, pTerm);
      } else {
         BinSumsInteractionDimensions<cCompilerClasses, cCompilerDimensionsPossible + 1>::Func(pInteractionShell, pTerm);
      }
   }
};

template<ptrdiff_t cCompilerClasses>
class BinSumsInteractionDimensions<cCompilerClasses, k_cCompilerOptimizedCountDimensionsMax + 1> final {
public:

   BinSumsInteractionDimensions() = delete; // this is a static class.  Do not construct

   INLINE_ALWAYS static void Func(InteractionShell * const pInteractionShell, const Term * const pTerm) {
      EBM_ASSERT(1 <= pTerm->GetCountSignificantDimensions());
      EBM_ASSERT(pTerm->GetCountSignificantDimensions() <= k_cDimensionsMax);
      BinSumsInteractionInternal<cCompilerClasses, k_dynamicDimensions>::Func(pInteractionShell, pTerm);
   }
};

template<ptrdiff_t cPossibleClasses>
class BinSumsInteractionTarget final {
public:

   BinSumsInteractionTarget() = delete; // this is a static class.  Do not construct

   INLINE_ALWAYS static void Func(InteractionShell * const pInteractionShell, const Term * const pTerm) {
      static_assert(IsClassification(cPossibleClasses), "cPossibleClasses needs to be a classification");
      static_assert(cPossibleClasses <= k_cCompilerClassesMax, "We can't have this many items in a data pack.");

      InteractionCore * const pInteractionCore = pInteractionShell->GetInteractionCore();
      const ptrdiff_t cRuntimeClasses = pInteractionCore->GetCountClasses();
      EBM_ASSERT(IsClassification(cRuntimeClasses));
      EBM_ASSERT(cRuntimeClasses <= k_cCompilerClassesMax);

      if(cPossibleClasses == cRuntimeClasses) {
         BinSumsInteractionDimensions<cPossibleClasses, 2>::Func(pInteractionShell, pTerm);
      } else {
         BinSumsInteractionTarget<cPossibleClasses + 1>::Func(pInteractionShell, pTerm);
      }
   }
};

template<>
class BinSumsInteractionTarget<k_cCompilerClassesMax + 1> final {
public:

   BinSumsInteractionTarget() = delete; // this is a static class.  Do not construct

   INLINE_ALWAYS static void Func(InteractionShell * const pInteractionShell, const Term * const pTerm) {
      static_assert(IsClassification(k_cCompilerClassesMax), "k_cCompilerClassesMax needs to be a classification");

      EBM_ASSERT(IsClassification(pInteractionShell->GetInteractionCore()->GetCountClasses()));
      EBM_ASSERT(k_cCompilerClassesMax < pInteractionShell->GetInteractionCore()->GetCountClasses());

      BinSumsInteractionDimensions<k_dynamicClassification, 2>::Func(pInteractionShell, pTerm);
   }
};

extern void BinSumsInteraction(InteractionShell * const pInteractionShell, const Term * const pTerm) {
   InteractionCore * const pInteractionCore = pInteractionShell->GetInteractionCore();
   const ptrdiff_t cRuntimeClasses = pInteractionCore->GetCountClasses();

   if(IsClassification(cRuntimeClasses)) {
      BinSumsInteractionTarget<2>::Func(pInteractionShell, pTerm);
   } else {
      EBM_ASSERT(IsRegression(cRuntimeClasses));
      BinSumsInteractionDimensions<k_regression, 2>::Func(pInteractionShell, pTerm);
   }
}

} // DEFINED_ZONE_NAME
