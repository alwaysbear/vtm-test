/* The copyright in this software is being made available under the BSD
 * License, included below. This software may be subject to other third party
 * and contributor rights, including patent rights, and no such rights are
 * granted under this license.
 *
 * Copyright (c) 2010-2019, ITU/ISO/IEC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *  * Neither the name of the ITU/ISO/IEC nor the names of its contributors may
 *    be used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

/** \file     AdaptiveLoopFilter.cpp
    \brief    adaptive loop filter class
*/

#include "AdaptiveLoopFilter.h"

#include "CodingStructure.h"
#include "Picture.h"
#if JVET_N0242_NON_LINEAR_ALF
#include <array>
#include <cmath>
#endif

#if JVET_N0242_NON_LINEAR_ALF
constexpr int AdaptiveLoopFilter::AlfNumClippingValues[];
#endif

AdaptiveLoopFilter::AdaptiveLoopFilter()
  : m_classifier( nullptr )
{
  for( int i = 0; i < NUM_DIRECTIONS; i++ )
  {
    m_laplacian[i] = nullptr;
  }

  for( int compIdx = 0; compIdx < MAX_NUM_COMPONENT; compIdx++ )
  {
    m_ctuEnableFlag[compIdx] = nullptr;
  }

  m_deriveClassificationBlk = deriveClassificationBlk;
  m_filter5x5Blk = filterBlk<ALF_FILTER_5>;
  m_filter7x7Blk = filterBlk<ALF_FILTER_7>;

#if ENABLE_SIMD_OPT_ALF
#ifdef TARGET_SIMD_X86
  initAdaptiveLoopFilterX86();
#endif
#endif
}

void AdaptiveLoopFilter::ALFProcess( CodingStructure& cs, AlfSliceParam& alfSliceParam )
{
  if( !alfSliceParam.enabledFlag[COMPONENT_Y] && !alfSliceParam.enabledFlag[COMPONENT_Cb] && !alfSliceParam.enabledFlag[COMPONENT_Cr] )
  {
    return;
  }

  // set available filter shapes
  alfSliceParam.filterShapes = m_filterShapes;

  // set clipping range
  m_clpRngs = cs.slice->getClpRngs();

  // set CTU enable flags
  for( int compIdx = 0; compIdx < MAX_NUM_COMPONENT; compIdx++ )
  {
    m_ctuEnableFlag[compIdx] = cs.picture->getAlfCtuEnableFlag( compIdx );
  }
  reconstructCoeff( alfSliceParam, CHANNEL_TYPE_LUMA );
#if JVET_N0242_NON_LINEAR_ALF
  if( alfSliceParam.enabledFlag[COMPONENT_Cb] || alfSliceParam.enabledFlag[COMPONENT_Cr] )
#endif
  reconstructCoeff( alfSliceParam, CHANNEL_TYPE_CHROMA );

  PelUnitBuf recYuv = cs.getRecoBuf();
  m_tempBuf.copyFrom( recYuv );
  PelUnitBuf tmpYuv = m_tempBuf.getBuf( cs.area );
  tmpYuv.extendBorderPel( MAX_ALF_FILTER_LENGTH >> 1 );

  const PreCalcValues& pcv = *cs.pcv;

  int ctuIdx = 0;
  for( int yPos = 0; yPos < pcv.lumaHeight; yPos += pcv.maxCUHeight )
  {
    for( int xPos = 0; xPos < pcv.lumaWidth; xPos += pcv.maxCUWidth )
    {
      const int width = ( xPos + pcv.maxCUWidth > pcv.lumaWidth ) ? ( pcv.lumaWidth - xPos ) : pcv.maxCUWidth;
      const int height = ( yPos + pcv.maxCUHeight > pcv.lumaHeight ) ? ( pcv.lumaHeight - yPos ) : pcv.maxCUHeight;
      const UnitArea area( cs.area.chromaFormat, Area( xPos, yPos, width, height ) );
      if( m_ctuEnableFlag[COMPONENT_Y][ctuIdx] )
      {
        Area blk( xPos, yPos, width, height );
        deriveClassification( m_classifier, tmpYuv.get( COMPONENT_Y ), blk );
        Area blkPCM(xPos, yPos, width, height);
        resetPCMBlkClassInfo(cs, m_classifier, tmpYuv.get(COMPONENT_Y), blkPCM);
#if JVET_N0180_ALF_LINE_BUFFER_REDUCTION
#if JVET_N0242_NON_LINEAR_ALF
        m_filter7x7Blk(m_classifier, recYuv, tmpYuv, blk, COMPONENT_Y, m_coeffFinal, m_clippFinal, m_clpRngs.comp[COMPONENT_Y], cs
          , m_alfVBLumaCTUHeight
          , ((yPos + pcv.maxCUHeight >= pcv.lumaHeight) ? pcv.lumaHeight : m_alfVBLumaPos)
        );
#else
        m_filter7x7Blk(m_classifier, recYuv, tmpYuv, blk, COMPONENT_Y, m_coeffFinal, m_clpRngs.comp[COMPONENT_Y], cs
          , m_alfVBLumaCTUHeight
          , ((yPos + pcv.maxCUHeight >= pcv.lumaHeight) ? pcv.lumaHeight : m_alfVBLumaPos)
        );
#endif
#else
#if JVET_N0242_NON_LINEAR_ALF
        m_filter7x7Blk(m_classifier, recYuv, tmpYuv, blk, COMPONENT_Y, m_coeffFinal, m_clippFinal, m_clpRngs.comp[COMPONENT_Y], cs);
#else
        m_filter7x7Blk(m_classifier, recYuv, tmpYuv, blk, COMPONENT_Y, m_coeffFinal, m_clpRngs.comp[COMPONENT_Y], cs);
#endif
#endif
      }

      for( int compIdx = 1; compIdx < MAX_NUM_COMPONENT; compIdx++ )
      {
        ComponentID compID = ComponentID( compIdx );
        const int chromaScaleX = getComponentScaleX( compID, tmpYuv.chromaFormat );
        const int chromaScaleY = getComponentScaleY( compID, tmpYuv.chromaFormat );

        if( m_ctuEnableFlag[compIdx][ctuIdx] )
        {
          Area blk( xPos >> chromaScaleX, yPos >> chromaScaleY, width >> chromaScaleX, height >> chromaScaleY );
#if JVET_N0180_ALF_LINE_BUFFER_REDUCTION
#if JVET_N0242_NON_LINEAR_ALF
          m_filter5x5Blk(m_classifier, recYuv, tmpYuv, blk, compID, alfSliceParam.chromaCoeff, m_chromaClippFinal, m_clpRngs.comp[compIdx], cs
            , m_alfVBChmaCTUHeight
            , ((yPos + pcv.maxCUHeight >= pcv.lumaHeight) ? pcv.lumaHeight : m_alfVBChmaPos)
          );
#else
          m_filter5x5Blk(m_classifier, recYuv, tmpYuv, blk, compID, alfSliceParam.chromaCoeff, m_clpRngs.comp[compIdx], cs
            , m_alfVBChmaCTUHeight
            , ((yPos + pcv.maxCUHeight >= pcv.lumaHeight) ? pcv.lumaHeight : m_alfVBChmaPos)
          );
#endif
#else

#if JVET_N0242_NON_LINEAR_ALF
          m_filter5x5Blk( m_classifier, recYuv, tmpYuv, blk, compID, alfSliceParam.chromaCoeff, m_chromaClippFinal, m_clpRngs.comp[compIdx], cs );
#else
          m_filter5x5Blk( m_classifier, recYuv, tmpYuv, blk, compID, alfSliceParam.chromaCoeff, m_clpRngs.comp[compIdx], cs );
#endif
#endif
        }
      }
      ctuIdx++;
    }
  }
}

void AdaptiveLoopFilter::reconstructCoeff( AlfSliceParam& alfSliceParam, ChannelType channel, const bool bRedo )
{
  int factor = ( 1 << ( m_NUM_BITS - 1 ) );
  AlfFilterType filterType = isLuma( channel ) ? ALF_FILTER_7 : ALF_FILTER_5;
  int numClasses = isLuma( channel ) ? MAX_NUM_ALF_CLASSES : 1;
  int numCoeff = filterType == ALF_FILTER_5 ? 7 : 13;
  int numCoeffMinus1 = numCoeff - 1;
  int numFilters = isLuma( channel ) ? alfSliceParam.numLumaFilters : 1;
  short* coeff = isLuma( channel ) ? alfSliceParam.lumaCoeff : alfSliceParam.chromaCoeff;
#if JVET_N0242_NON_LINEAR_ALF
  short* clipp = isLuma( channel ) ? alfSliceParam.lumaClipp : alfSliceParam.chromaClipp;
#endif

  if( alfSliceParam.alfLumaCoeffDeltaPredictionFlag && isLuma( channel ) )
  {
    for( int i = 1; i < numFilters; i++ )
    {
      for( int j = 0; j < numCoeffMinus1; j++ )
      {
        coeff[i * MAX_NUM_ALF_LUMA_COEFF + j] += coeff[( i - 1 ) * MAX_NUM_ALF_LUMA_COEFF + j];
      }
    }
  }

  for( int filterIdx = 0; filterIdx < numFilters; filterIdx++ )
  {
#if JVET_N0242_NON_LINEAR_ALF
    coeff[filterIdx* MAX_NUM_ALF_LUMA_COEFF + numCoeffMinus1] = factor;
#else
    int sum = 0;
    for( int i = 0; i < numCoeffMinus1; i++ )
    {
      sum += ( coeff[filterIdx* MAX_NUM_ALF_LUMA_COEFF + i] << 1 );
    }
    coeff[filterIdx* MAX_NUM_ALF_LUMA_COEFF + numCoeffMinus1] = factor - sum;
#endif
  }

  if( isChroma( channel ) )
  {
#if JVET_N0242_NON_LINEAR_ALF
    for( int coeffIdx = 0; coeffIdx < numCoeffMinus1; ++coeffIdx )
    {
      m_chromaClippFinal[coeffIdx] = alfSliceParam.nonLinearFlag[channel] ? m_alfClippingValues[channel][clipp[coeffIdx]] : m_alfClippingValues[channel][0];
    }
#endif
    return;
  }

  for( int classIdx = 0; classIdx < numClasses; classIdx++ )
  {
    int filterIdx = alfSliceParam.filterCoeffDeltaIdx[classIdx];
    memcpy( m_coeffFinal + classIdx * MAX_NUM_ALF_LUMA_COEFF, coeff + filterIdx * MAX_NUM_ALF_LUMA_COEFF, sizeof( short ) * numCoeff );
#if JVET_N0242_NON_LINEAR_ALF
    for( int coeffIdx = 0; coeffIdx < numCoeffMinus1; ++coeffIdx )
    {
      (m_clippFinal + classIdx * MAX_NUM_ALF_LUMA_COEFF)[coeffIdx] = alfSliceParam.nonLinearFlag[channel] ? m_alfClippingValues[channel][(clipp + filterIdx * MAX_NUM_ALF_LUMA_COEFF)[coeffIdx]] : m_alfClippingValues[channel][0];
    }
#endif
  }

  if( bRedo && alfSliceParam.alfLumaCoeffDeltaPredictionFlag )
  {
    for( int i = numFilters - 1; i > 0; i-- )
    {
      for( int j = 0; j < numCoeffMinus1; j++ )
      {
        coeff[i * MAX_NUM_ALF_LUMA_COEFF + j] = coeff[i * MAX_NUM_ALF_LUMA_COEFF + j] - coeff[( i - 1 ) * MAX_NUM_ALF_LUMA_COEFF + j];
      }
    }
  }
}

void AdaptiveLoopFilter::create( const int picWidth, const int picHeight, const ChromaFormat format, const int maxCUWidth, const int maxCUHeight, const int maxCUDepth, const int inputBitDepth[MAX_NUM_CHANNEL_TYPE] )
{
  std::memcpy( m_inputBitDepth, inputBitDepth, sizeof( m_inputBitDepth ) );
  m_picWidth = picWidth;
  m_picHeight = picHeight;
  m_maxCUWidth = maxCUWidth;
  m_maxCUHeight = maxCUHeight;
  m_maxCUDepth = maxCUDepth;
  m_chromaFormat = format;

  m_numCTUsInWidth = ( m_picWidth / m_maxCUWidth ) + ( ( m_picWidth % m_maxCUWidth ) ? 1 : 0 );
  m_numCTUsInHeight = ( m_picHeight / m_maxCUHeight ) + ( ( m_picHeight % m_maxCUHeight ) ? 1 : 0 );
  m_numCTUsInPic = m_numCTUsInHeight * m_numCTUsInWidth;
  m_filterShapes[CHANNEL_TYPE_LUMA].push_back( AlfFilterShape( 7 ) );
  m_filterShapes[CHANNEL_TYPE_CHROMA].push_back( AlfFilterShape( 5 ) );
#if JVET_N0180_ALF_LINE_BUFFER_REDUCTION
  m_alfVBLumaPos = m_maxCUHeight - JVET_N0180_ALF_VB_POS_ABOVE_CTUROW_LUMA;
  m_alfVBChmaPos = (m_maxCUHeight >> ((m_chromaFormat == CHROMA_420) ? 1 : 0)) - JVET_N0180_ALF_VB_POS_ABOVE_CTUROW_CHMA;

  m_alfVBLumaCTUHeight = m_maxCUHeight;
  m_alfVBChmaCTUHeight = (m_maxCUHeight >> ((m_chromaFormat == CHROMA_420) ? 1 : 0));
#endif

#if JVET_N0242_NON_LINEAR_ALF
  static_assert( AlfNumClippingValues[CHANNEL_TYPE_LUMA] > 0, "AlfNumClippingValues[CHANNEL_TYPE_LUMA] must be at least one" );
  for( int i = 0; i < AlfNumClippingValues[CHANNEL_TYPE_LUMA]; ++i )
  {
    m_alfClippingValues[CHANNEL_TYPE_LUMA][i] =
      (Pel) std::round(
        std::pow(
          2.,
          double( m_inputBitDepth[CHANNEL_TYPE_LUMA] * ( AlfNumClippingValues[CHANNEL_TYPE_LUMA] - i ) ) / AlfNumClippingValues[CHANNEL_TYPE_LUMA]
          ) );
  }
  static_assert( AlfNumClippingValues[CHANNEL_TYPE_CHROMA] > 0, "AlfNumClippingValues[CHANNEL_TYPE_CHROMA] must be at least one" );
  m_alfClippingValues[CHANNEL_TYPE_CHROMA][0] = 1 << m_inputBitDepth[CHANNEL_TYPE_CHROMA];
  for( int i = 1; i < AlfNumClippingValues[CHANNEL_TYPE_CHROMA]; ++i )
  {
    m_alfClippingValues[CHANNEL_TYPE_CHROMA][i] =
      (Pel) std::round(
        std::pow(
          2.,
          m_inputBitDepth[CHANNEL_TYPE_CHROMA] - 8
            + 8. * ( AlfNumClippingValues[CHANNEL_TYPE_CHROMA] - i - 1 ) / ( AlfNumClippingValues[CHANNEL_TYPE_CHROMA] - 1 )
          ) );
  }
#endif

  m_tempBuf.destroy();
  m_tempBuf.create( format, Area( 0, 0, picWidth, picHeight ), maxCUWidth, MAX_ALF_FILTER_LENGTH >> 1, 0, false );

  // Laplacian based activity
  for( int i = 0; i < NUM_DIRECTIONS; i++ )
  {
    if ( m_laplacian[i] == nullptr )
    {
      m_laplacian[i] = new int*[m_CLASSIFICATION_BLK_SIZE + 5];

      for( int y = 0; y < m_CLASSIFICATION_BLK_SIZE + 5; y++ )
      {
        m_laplacian[i][y] = new int[m_CLASSIFICATION_BLK_SIZE + 5];
      }
    }
  }

  // Classification
  if ( m_classifier == nullptr )
  {
    m_classifier = new AlfClassifier*[picHeight];
    for( int i = 0; i < picHeight; i++ )
    {
      m_classifier[i] = new AlfClassifier[picWidth];
    }
  }
}

void AdaptiveLoopFilter::destroy()
{
  for( int i = 0; i < NUM_DIRECTIONS; i++ )
  {
    if( m_laplacian[i] )
    {
      for( int y = 0; y < m_CLASSIFICATION_BLK_SIZE + 5; y++ )
      {
        delete[] m_laplacian[i][y];
        m_laplacian[i][y] = nullptr;
      }

      delete[] m_laplacian[i];
      m_laplacian[i] = nullptr;
    }
  }

  if( m_classifier )
  {
    for( int i = 0; i < m_picHeight; i++ )
    {
      delete[] m_classifier[i];
      m_classifier[i] = nullptr;
    }

    delete[] m_classifier;
    m_classifier = nullptr;
  }

  m_tempBuf.destroy();
}

void AdaptiveLoopFilter::deriveClassification( AlfClassifier** classifier, const CPelBuf& srcLuma, const Area& blk )
{
  int height = blk.pos().y + blk.height;
  int width = blk.pos().x + blk.width;

  for( int i = blk.pos().y; i < height; i += m_CLASSIFICATION_BLK_SIZE )
  {
    int nHeight = std::min( i + m_CLASSIFICATION_BLK_SIZE, height ) - i;

    for( int j = blk.pos().x; j < width; j += m_CLASSIFICATION_BLK_SIZE )
    {
      int nWidth = std::min( j + m_CLASSIFICATION_BLK_SIZE, width ) - j;
#if JVET_N0180_ALF_LINE_BUFFER_REDUCTION   
      m_deriveClassificationBlk(classifier, m_laplacian, srcLuma, Area(j, i, nWidth, nHeight), m_inputBitDepth[CHANNEL_TYPE_LUMA] + 4
        , m_alfVBLumaCTUHeight
        , ((i + nHeight >= m_picHeight) ? m_picHeight : m_alfVBLumaPos)
      );
#else
      m_deriveClassificationBlk(classifier, m_laplacian, srcLuma, Area(j, i, nWidth, nHeight), m_inputBitDepth[CHANNEL_TYPE_LUMA] + 4);
#endif
     
    }
  }
}
void AdaptiveLoopFilter::resetPCMBlkClassInfo(CodingStructure & cs,  AlfClassifier** classifier, const CPelBuf& srcLuma, const Area& blk)
{
  if ( !cs.sps->getPCMFilterDisableFlag() )
  {
    return;
  }

  int height = blk.pos().y + blk.height;
  int width = blk.pos().x + blk.width;
  const int clsSizeY = 4;
  const int clsSizeX = 4;
  int classIdx = m_ALF_UNUSED_CLASSIDX;
  int transposeIdx = m_ALF_UNUSED_TRANSPOSIDX;

  for( int i = blk.pos().y; i < height; i += m_CLASSIFICATION_BLK_SIZE )
  {
    int nHeight = std::min(i + m_CLASSIFICATION_BLK_SIZE, height) - i;

    for( int j = blk.pos().x; j < width; j += m_CLASSIFICATION_BLK_SIZE )
    {
      int nWidth = std::min(j + m_CLASSIFICATION_BLK_SIZE, width) - j;
      int posX = j;
      int posY = i;

      for( int subi = 0; subi < nHeight; subi += clsSizeY )
      {
        for( int subj = 0; subj < nWidth; subj += clsSizeX )
        {
          int yOffset = subi + posY;
          int xOffset = subj + posX;
          Position pos(xOffset, yOffset);

          const CodingUnit* cu = cs.getCU(pos, CH_L);
          if ( cu->ipcm )
          {
            AlfClassifier *cl0 = classifier[yOffset] + xOffset;
            AlfClassifier *cl1 = classifier[yOffset + 1] + xOffset;
            AlfClassifier *cl2 = classifier[yOffset + 2] + xOffset;
            AlfClassifier *cl3 = classifier[yOffset + 3] + xOffset;
            cl0[0] = cl0[1] = cl0[2] = cl0[3] =
            cl1[0] = cl1[1] = cl1[2] = cl1[3] =
            cl2[0] = cl2[1] = cl2[2] = cl2[3] =
            cl3[0] = cl3[1] = cl3[2] = cl3[3] = AlfClassifier(classIdx, transposeIdx);
          }
        }
      }
    }
  }
}

#if JVET_N0180_ALF_LINE_BUFFER_REDUCTION  
void AdaptiveLoopFilter::deriveClassificationBlk(AlfClassifier** classifier, int** laplacian[NUM_DIRECTIONS], const CPelBuf& srcLuma, const Area& blk, const int shift,  int vbCTUHeight, int vbPos)
#else
void AdaptiveLoopFilter::deriveClassificationBlk(AlfClassifier** classifier, int** laplacian[NUM_DIRECTIONS], const CPelBuf& srcLuma, const Area& blk, const int shift)
#endif
{
  static const int th[16] = { 0, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 4 };
  const int stride = srcLuma.stride;
  const Pel* src = srcLuma.buf;
  const int maxActivity = 15;

  int fl = 2;
  int flP1 = fl + 1;
  int fl2 = 2 * fl;

  int mainDirection, secondaryDirection, dirTempHV, dirTempD;

  int pixY;
  int height = blk.height + fl2;
  int width = blk.width + fl2;
  int posX = blk.pos().x;
  int posY = blk.pos().y;
  int startHeight = posY - flP1;

  for( int i = 0; i < height; i += 2 )
  {
    int yoffset = ( i + 1 + startHeight ) * stride - flP1;
    const Pel *src0 = &src[yoffset - stride];
    const Pel *src1 = &src[yoffset];
    const Pel *src2 = &src[yoffset + stride];
    const Pel *src3 = &src[yoffset + stride * 2];
#if JVET_N0180_ALF_LINE_BUFFER_REDUCTION    
    if (((posY - 2 + i) > 0) && ((posY - 2 + i) % vbCTUHeight) == (vbPos - 2))
    {
      src3 = &src[yoffset + stride];
    }
    else if (((posY - 2 + i) > 0) && ((posY - 2 + i) % vbCTUHeight) == vbPos)
    {
      src0 = &src[yoffset];
    }
#endif 
    int* pYver = laplacian[VER][i];
    int* pYhor = laplacian[HOR][i];
    int* pYdig0 = laplacian[DIAG0][i];
    int* pYdig1 = laplacian[DIAG1][i];

    for( int j = 0; j < width; j += 2 )
    {
      pixY = j + 1 + posX;
      const Pel *pY = src1 + pixY;
      const Pel* pYdown = src0 + pixY;
      const Pel* pYup = src2 + pixY;
      const Pel* pYup2 = src3 + pixY;

      const Pel y0 = pY[0] << 1;
      const Pel yup1 = pYup[1] << 1;

      pYver[j] = abs( y0 - pYdown[0] - pYup[0] ) + abs( yup1 - pY[1] - pYup2[1] );
      pYhor[j] = abs( y0 - pY[1] - pY[-1] ) + abs( yup1 - pYup[2] - pYup[0] );
      pYdig0[j] = abs( y0 - pYdown[-1] - pYup[1] ) + abs( yup1 - pY[0] - pYup2[2] );
      pYdig1[j] = abs( y0 - pYup[-1] - pYdown[1] ) + abs( yup1 - pYup2[0] - pY[2] );

      if( j > 4 && ( j - 6 ) % 4 == 0 )
      {
        int jM6 = j - 6;
        int jM4 = j - 4;
        int jM2 = j - 2;

        pYver[jM6] += pYver[jM4] + pYver[jM2] + pYver[j];
        pYhor[jM6] += pYhor[jM4] + pYhor[jM2] + pYhor[j];
        pYdig0[jM6] += pYdig0[jM4] + pYdig0[jM2] + pYdig0[j];
        pYdig1[jM6] += pYdig1[jM4] + pYdig1[jM2] + pYdig1[j];
      }
    }
  }

  // classification block size
  const int clsSizeY = 4;
  const int clsSizeX = 4;

  for( int i = 0; i < blk.height; i += clsSizeY )
  {
    int* pYver = laplacian[VER][i];
    int* pYver2 = laplacian[VER][i + 2];
    int* pYver4 = laplacian[VER][i + 4];
    int* pYver6 = laplacian[VER][i + 6];

    int* pYhor = laplacian[HOR][i];
    int* pYhor2 = laplacian[HOR][i + 2];
    int* pYhor4 = laplacian[HOR][i + 4];
    int* pYhor6 = laplacian[HOR][i + 6];

    int* pYdig0 = laplacian[DIAG0][i];
    int* pYdig02 = laplacian[DIAG0][i + 2];
    int* pYdig04 = laplacian[DIAG0][i + 4];
    int* pYdig06 = laplacian[DIAG0][i + 6];

    int* pYdig1 = laplacian[DIAG1][i];
    int* pYdig12 = laplacian[DIAG1][i + 2];
    int* pYdig14 = laplacian[DIAG1][i + 4];
    int* pYdig16 = laplacian[DIAG1][i + 6];

    for( int j = 0; j < blk.width; j += clsSizeX )
    {
#if JVET_N0180_ALF_LINE_BUFFER_REDUCTION
      int sumV = 0; int sumH = 0; int sumD0 = 0; int sumD1 = 0;
      if (((i + posY) % vbCTUHeight) == (vbPos - 4))
      {
        sumV = pYver[j] + pYver2[j] + pYver4[j];
        sumH = pYhor[j] + pYhor2[j] + pYhor4[j];
        sumD0 = pYdig0[j] + pYdig02[j] + pYdig04[j];
        sumD1 = pYdig1[j] + pYdig12[j] + pYdig14[j];
      }
      else if (((i + posY) % vbCTUHeight) == vbPos)
      {
        sumV = pYver2[j] + pYver4[j] + pYver6[j];
        sumH = pYhor2[j] + pYhor4[j] + pYhor6[j];
        sumD0 = pYdig02[j] + pYdig04[j] + pYdig06[j];
        sumD1 = pYdig12[j] + pYdig14[j] + pYdig16[j];
      }
      else
      {
        sumV = pYver[j] + pYver2[j] + pYver4[j] + pYver6[j];
        sumH = pYhor[j] + pYhor2[j] + pYhor4[j] + pYhor6[j];
        sumD0 = pYdig0[j] + pYdig02[j] + pYdig04[j] + pYdig06[j];
        sumD1 = pYdig1[j] + pYdig12[j] + pYdig14[j] + pYdig16[j];
      }
#else
      int sumV = pYver[j] + pYver2[j] + pYver4[j] + pYver6[j];
      int sumH = pYhor[j] + pYhor2[j] + pYhor4[j] + pYhor6[j];
      int sumD0 = pYdig0[j] + pYdig02[j] + pYdig04[j] + pYdig06[j];
      int sumD1 = pYdig1[j] + pYdig12[j] + pYdig14[j] + pYdig16[j];
#endif

      int tempAct = sumV + sumH;
#if JVET_N0180_ALF_LINE_BUFFER_REDUCTION
      int activity = 0;
      if ((((i + posY) % vbCTUHeight) == (vbPos - 4)) || (((i + posY) % vbCTUHeight) == vbPos))
      {
        activity = (Pel)Clip3<int>(0, maxActivity, (tempAct * 96) >> shift);
      }
      else
      {
        activity = (Pel)Clip3<int>(0, maxActivity, (tempAct * 64) >> shift);
      }
#else
      int activity = (Pel)Clip3<int>(0, maxActivity, (tempAct * 64) >> shift);
#endif
      int classIdx = th[activity];

      int hv1, hv0, d1, d0, hvd1, hvd0;

      if( sumV > sumH )
      {
        hv1 = sumV;
        hv0 = sumH;
        dirTempHV = 1;
      }
      else
      {
        hv1 = sumH;
        hv0 = sumV;
        dirTempHV = 3;
      }
      if( sumD0 > sumD1 )
      {
        d1 = sumD0;
        d0 = sumD1;
        dirTempD = 0;
      }
      else
      {
        d1 = sumD1;
        d0 = sumD0;
        dirTempD = 2;
      }
      if( d1*hv0 > hv1*d0 )
      {
        hvd1 = d1;
        hvd0 = d0;
        mainDirection = dirTempD;
        secondaryDirection = dirTempHV;
      }
      else
      {
        hvd1 = hv1;
        hvd0 = hv0;
        mainDirection = dirTempHV;
        secondaryDirection = dirTempD;
      }

      int directionStrength = 0;
      if( hvd1 > 2 * hvd0 )
      {
        directionStrength = 1;
      }
      if( hvd1 * 2 > 9 * hvd0 )
      {
        directionStrength = 2;
      }

      if( directionStrength )
      {
        classIdx += ( ( ( mainDirection & 0x1 ) << 1 ) + directionStrength ) * 5;
      }

      static const int transposeTable[8] = { 0, 1, 0, 2, 2, 3, 1, 3 };
      int transposeIdx = transposeTable[mainDirection * 2 + ( secondaryDirection >> 1 )];

      int yOffset = i + posY;
      int xOffset = j + posX;

      AlfClassifier *cl0 = classifier[yOffset] + xOffset;
      AlfClassifier *cl1 = classifier[yOffset + 1] + xOffset;
      AlfClassifier *cl2 = classifier[yOffset + 2] + xOffset;
      AlfClassifier *cl3 = classifier[yOffset + 3] + xOffset;
      cl0[0] = cl0[1] = cl0[2] = cl0[3] = cl1[0] = cl1[1] = cl1[2] = cl1[3] = cl2[0] = cl2[1] = cl2[2] = cl2[3] = cl3[0] = cl3[1] = cl3[2] = cl3[3] = AlfClassifier( classIdx, transposeIdx );
    }
  }
}

template<AlfFilterType filtType>
#if JVET_N0180_ALF_LINE_BUFFER_REDUCTION
#if JVET_N0242_NON_LINEAR_ALF
void AdaptiveLoopFilter::filterBlk(AlfClassifier** classifier, const PelUnitBuf &recDst, const CPelUnitBuf& recSrc, const Area& blk, const ComponentID compId, short* filterSet, short* fClipSet, const ClpRng& clpRng, CodingStructure& cs, int vbCTUHeight, int vbPos)
#else
void AdaptiveLoopFilter::filterBlk(AlfClassifier** classifier, const PelUnitBuf &recDst, const CPelUnitBuf& recSrc, const Area& blk, const ComponentID compId, short* filterSet, const ClpRng& clpRng, CodingStructure& cs, int vbCTUHeight, int vbPos)
#endif
#else
#if JVET_N0242_NON_LINEAR_ALF
void AdaptiveLoopFilter::filterBlk(AlfClassifier** classifier, const PelUnitBuf &recDst, const CPelUnitBuf& recSrc, const Area& blk, const ComponentID compId, short* filterSet, short* fClipSet, const ClpRng& clpRng, CodingStructure& cs)
#else
void AdaptiveLoopFilter::filterBlk(AlfClassifier** classifier, const PelUnitBuf &recDst, const CPelUnitBuf& recSrc, const Area& blk, const ComponentID compId, short* filterSet, const ClpRng& clpRng, CodingStructure& cs)
#endif
#endif

{
  const bool bChroma = isChroma( compId );
  if( bChroma )
  {
    CHECK( filtType != 0, "Chroma needs to have filtType == 0" );
  }
  const SPS*     sps = cs.slice->getSPS();
  bool isDualTree =CS::isDualITree(cs);
  bool isPCMFilterDisabled = sps->getPCMFilterDisableFlag();
  ChromaFormat nChromaFormat = sps->getChromaFormatIdc();

  const CPelBuf srcLuma = recSrc.get( compId );
  PelBuf dstLuma = recDst.get( compId );

  const int srcStride = srcLuma.stride;
  const int dstStride = dstLuma.stride;

  const int startHeight = blk.y;
  const int endHeight = blk.y + blk.height;
  const int startWidth = blk.x;
  const int endWidth = blk.x + blk.width;

  const Pel* src = srcLuma.buf;
  Pel* dst = dstLuma.buf + startHeight * dstStride;

  const Pel *pImgYPad0, *pImgYPad1, *pImgYPad2, *pImgYPad3, *pImgYPad4, *pImgYPad5, *pImgYPad6;
  const Pel *pImg0, *pImg1, *pImg2, *pImg3, *pImg4, *pImg5, *pImg6;

  short *coef = filterSet;
#if JVET_N0242_NON_LINEAR_ALF
  short *clip = fClipSet;
#endif

  const int shift = m_NUM_BITS - 1;

  const int offset = 1 << ( shift - 1 );

  int transposeIdx = 0;
  const int clsSizeY = 4;
  const int clsSizeX = 4;

  bool pcmFlags2x2[4] = {0,0,0,0};

  CHECK( startHeight % clsSizeY, "Wrong startHeight in filtering" );
  CHECK( startWidth % clsSizeX, "Wrong startWidth in filtering" );
  CHECK( ( endHeight - startHeight ) % clsSizeY, "Wrong endHeight in filtering" );
  CHECK( ( endWidth - startWidth ) % clsSizeX, "Wrong endWidth in filtering" );

  AlfClassifier *pClass = nullptr;

  int dstStride2 = dstStride * clsSizeY;
  int srcStride2 = srcStride * clsSizeY;

#if JVET_N0242_NON_LINEAR_ALF
  std::array<int, MAX_NUM_ALF_LUMA_COEFF> filterCoeff;
  std::array<int, MAX_NUM_ALF_LUMA_COEFF> filterClipp;
#else
  std::vector<Pel> filterCoeff( MAX_NUM_ALF_LUMA_COEFF );
#endif

  pImgYPad0 = src + startHeight * srcStride + startWidth;
  pImgYPad1 = pImgYPad0 + srcStride;
  pImgYPad2 = pImgYPad0 - srcStride;
  pImgYPad3 = pImgYPad1 + srcStride;
  pImgYPad4 = pImgYPad2 - srcStride;
  pImgYPad5 = pImgYPad3 + srcStride;
  pImgYPad6 = pImgYPad4 - srcStride;

  Pel* pRec0 = dst + startWidth;
  Pel* pRec1 = pRec0 + dstStride;

  for( int i = 0; i < endHeight - startHeight; i += clsSizeY )
  {
    if( !bChroma )
    {
      pClass = classifier[startHeight + i] + startWidth;
    }

    for( int j = 0; j < endWidth - startWidth; j += clsSizeX )
    {
      if( !bChroma )
      {
        AlfClassifier& cl = pClass[j];
        transposeIdx = cl.transposeIdx;
        if( isPCMFilterDisabled && cl.classIdx== m_ALF_UNUSED_CLASSIDX && transposeIdx== m_ALF_UNUSED_TRANSPOSIDX )
        {
          continue;
        }
        coef = filterSet + cl.classIdx * MAX_NUM_ALF_LUMA_COEFF;
#if JVET_N0242_NON_LINEAR_ALF
        clip = fClipSet + cl.classIdx * MAX_NUM_ALF_LUMA_COEFF;
#endif
      }
      else if( isPCMFilterDisabled )
      {
        int  blkX, blkY;
        bool *flags = pcmFlags2x2;

        // check which chroma 2x2 blocks use PCM
        // chroma PCM may not be aligned with 4x4 ALF processing grid
        for( blkY=0; blkY<4; blkY+=2 )
        {
          for( blkX=0; blkX<4; blkX+=2 )
          {
            Position pos(j+startWidth+blkX, i+startHeight+blkY);
            CodingUnit* cu = isDualTree ? cs.getCU(pos, CH_C) : cs.getCU(recalcPosition(nChromaFormat, CH_C, CH_L, pos), CH_L);
            *flags++ = cu->ipcm ? 1 : 0;
          }
        }

        // skip entire 4x4 if all chroma 2x2 blocks use PCM
        if( pcmFlags2x2[0] && pcmFlags2x2[1] && pcmFlags2x2[2] && pcmFlags2x2[3] )
        {
          continue;
        }
      }


      if( filtType == ALF_FILTER_7 )
      {
        if( transposeIdx == 1 )
        {
          filterCoeff = { coef[9], coef[4], coef[10], coef[8], coef[1], coef[5], coef[11], coef[7], coef[3], coef[0], coef[2], coef[6], coef[12] };
#if JVET_N0242_NON_LINEAR_ALF
          filterClipp = { clip[9], clip[4], clip[10], clip[8], clip[1], clip[5], clip[11], clip[7], clip[3], clip[0], clip[2], clip[6], clip[12] };
#endif
        }
        else if( transposeIdx == 2 )
        {
          filterCoeff = { coef[0], coef[3], coef[2], coef[1], coef[8], coef[7], coef[6], coef[5], coef[4], coef[9], coef[10], coef[11], coef[12] };
#if JVET_N0242_NON_LINEAR_ALF
          filterClipp = { clip[0], clip[3], clip[2], clip[1], clip[8], clip[7], clip[6], clip[5], clip[4], clip[9], clip[10], clip[11], clip[12] };
#endif
        }
        else if( transposeIdx == 3 )
        {
          filterCoeff = { coef[9], coef[8], coef[10], coef[4], coef[3], coef[7], coef[11], coef[5], coef[1], coef[0], coef[2], coef[6], coef[12] };
#if JVET_N0242_NON_LINEAR_ALF
          filterClipp = { clip[9], clip[8], clip[10], clip[4], clip[3], clip[7], clip[11], clip[5], clip[1], clip[0], clip[2], clip[6], clip[12] };
#endif
        }
        else
        {
          filterCoeff = { coef[0], coef[1], coef[2], coef[3], coef[4], coef[5], coef[6], coef[7], coef[8], coef[9], coef[10], coef[11], coef[12] };
#if JVET_N0242_NON_LINEAR_ALF
          filterClipp = { clip[0], clip[1], clip[2], clip[3], clip[4], clip[5], clip[6], clip[7], clip[8], clip[9], clip[10], clip[11], clip[12] };
#endif
        }
      }
      else
      {
        if( transposeIdx == 1 )
        {
          filterCoeff = { coef[4], coef[1], coef[5], coef[3], coef[0], coef[2], coef[6] };
#if JVET_N0242_NON_LINEAR_ALF
          filterClipp = { clip[4], clip[1], clip[5], clip[3], clip[0], clip[2], clip[6] };
#endif
        }
        else if( transposeIdx == 2 )
        {
          filterCoeff = { coef[0], coef[3], coef[2], coef[1], coef[4], coef[5], coef[6] };
#if JVET_N0242_NON_LINEAR_ALF
          filterClipp = { clip[0], clip[3], clip[2], clip[1], clip[4], clip[5], clip[6] };
#endif
        }
        else if( transposeIdx == 3 )
        {
          filterCoeff = { coef[4], coef[3], coef[5], coef[1], coef[0], coef[2], coef[6] };
#if JVET_N0242_NON_LINEAR_ALF
          filterClipp = { clip[4], clip[3], clip[5], clip[1], clip[0], clip[2], clip[6] };
#endif
        }
        else
        {
          filterCoeff = { coef[0], coef[1], coef[2], coef[3], coef[4], coef[5], coef[6] };
#if JVET_N0242_NON_LINEAR_ALF
          filterClipp = { clip[0], clip[1], clip[2], clip[3], clip[4], clip[5], clip[6] };
#endif
        }
      }

      for( int ii = 0; ii < clsSizeY; ii++ )
      {
        pImg0 = pImgYPad0 + j + ii * srcStride;
        pImg1 = pImgYPad1 + j + ii * srcStride;
        pImg2 = pImgYPad2 + j + ii * srcStride;
        pImg3 = pImgYPad3 + j + ii * srcStride;
        pImg4 = pImgYPad4 + j + ii * srcStride;
        pImg5 = pImgYPad5 + j + ii * srcStride;
        pImg6 = pImgYPad6 + j + ii * srcStride;

        pRec1 = pRec0 + j + ii * dstStride;

#if JVET_N0180_ALF_LINE_BUFFER_REDUCTION
        if ((startHeight + i + ii) % vbCTUHeight < vbPos && ((startHeight + i + ii) % vbCTUHeight >= vbPos - (bChroma ? 2 : 4))) //above
        {
          pImg1 = ((startHeight + i + ii) % vbCTUHeight == vbPos - 1) ? pImg0 : pImg1;
          pImg3 = ((startHeight + i + ii) % vbCTUHeight >= vbPos - 2) ? pImg1 : pImg3;
          pImg5 = ((startHeight + i + ii) % vbCTUHeight >= vbPos - 3) ? pImg3 : pImg5;

          pImg2 = ((startHeight + i + ii) % vbCTUHeight == vbPos - 1) ? pImg0 : pImg2;
          pImg4 = ((startHeight + i + ii) % vbCTUHeight >= vbPos - 2) ? pImg2 : pImg4;
          pImg6 = ((startHeight + i + ii) % vbCTUHeight >= vbPos - 3) ? pImg4 : pImg6;
        }
        else if ((startHeight + i + ii) % vbCTUHeight >= vbPos && ((startHeight + i + ii) % vbCTUHeight <= vbPos + (bChroma ? 1 : 3))) //bottom
        {
          pImg2 = ((startHeight + i + ii) % vbCTUHeight == vbPos) ? pImg0 : pImg2;
          pImg4 = ((startHeight + i + ii) % vbCTUHeight <= vbPos + 1) ? pImg2 : pImg4;
          pImg6 = ((startHeight + i + ii) % vbCTUHeight <= vbPos + 2) ? pImg4 : pImg6;

          pImg1 = ((startHeight + i + ii) % vbCTUHeight == vbPos) ? pImg0 : pImg1;
          pImg3 = ((startHeight + i + ii) % vbCTUHeight <= vbPos + 1) ? pImg1 : pImg3;
          pImg5 = ((startHeight + i + ii) % vbCTUHeight <= vbPos + 2) ? pImg3 : pImg5;
        }
#endif

        for( int jj = 0; jj < clsSizeX; jj++ )
        {

          // skip 2x2 PCM chroma blocks
          if( bChroma && isPCMFilterDisabled )
          {
            if( pcmFlags2x2[2*(ii>>1) + (jj>>1)] )
            {
              pImg0++;
              pImg1++;
              pImg2++;
              pImg3++;
              pImg4++;
              pImg5++;
              pImg6++;
              continue;
            }
          }

          int sum = 0;
#if JVET_N0242_NON_LINEAR_ALF
          const Pel curr = pImg0[+0];
#endif
          if( filtType == ALF_FILTER_7 )
          {
#if !JVET_N0242_NON_LINEAR_ALF
            sum += filterCoeff[0] * ( pImg5[0] + pImg6[0] );
#else
            sum += filterCoeff[0] * ( clipALF(filterClipp[0], curr, pImg5[+0], pImg6[+0]) );
#endif

#if !JVET_N0242_NON_LINEAR_ALF
            sum += filterCoeff[1] * ( pImg3[+1] + pImg4[-1] );
#else
            sum += filterCoeff[1] * ( clipALF(filterClipp[1], curr, pImg3[+1], pImg4[-1]) );
#endif
#if !JVET_N0242_NON_LINEAR_ALF
            sum += filterCoeff[2] * ( pImg3[+0] + pImg4[+0] );
#else
            sum += filterCoeff[2] * ( clipALF(filterClipp[2], curr, pImg3[+0], pImg4[+0]) );
#endif
#if !JVET_N0242_NON_LINEAR_ALF
            sum += filterCoeff[3] * ( pImg3[-1] + pImg4[+1] );
#else
            sum += filterCoeff[3] * ( clipALF(filterClipp[3], curr, pImg3[-1], pImg4[+1]) );
#endif

#if !JVET_N0242_NON_LINEAR_ALF
            sum += filterCoeff[4] * ( pImg1[+2] + pImg2[-2] );
#else
            sum += filterCoeff[4] * ( clipALF(filterClipp[4], curr, pImg1[+2], pImg2[-2]) );
#endif
#if !JVET_N0242_NON_LINEAR_ALF
            sum += filterCoeff[5] * ( pImg1[+1] + pImg2[-1] );
#else
            sum += filterCoeff[5] * ( clipALF(filterClipp[5], curr, pImg1[+1], pImg2[-1]) );
#endif
#if !JVET_N0242_NON_LINEAR_ALF
            sum += filterCoeff[6] * ( pImg1[+0] + pImg2[+0] );
#else
            sum += filterCoeff[6] * ( clipALF(filterClipp[6], curr, pImg1[+0], pImg2[+0]) );
#endif
#if !JVET_N0242_NON_LINEAR_ALF
            sum += filterCoeff[7] * ( pImg1[-1] + pImg2[+1] );
#else
            sum += filterCoeff[7] * ( clipALF(filterClipp[7], curr, pImg1[-1], pImg2[+1]) );
#endif
#if !JVET_N0242_NON_LINEAR_ALF
            sum += filterCoeff[8] * ( pImg1[-2] + pImg2[+2] );
#else
            sum += filterCoeff[8] * ( clipALF(filterClipp[8], curr, pImg1[-2], pImg2[+2]) );
#endif

#if !JVET_N0242_NON_LINEAR_ALF
            sum += filterCoeff[9] * ( pImg0[+3] + pImg0[-3] );
#else
            sum += filterCoeff[9] * ( clipALF(filterClipp[9], curr, pImg0[+3], pImg0[-3]) );
#endif
#if !JVET_N0242_NON_LINEAR_ALF
            sum += filterCoeff[10] * ( pImg0[+2] + pImg0[-2] );
#else
            sum += filterCoeff[10] * ( clipALF(filterClipp[10], curr, pImg0[+2], pImg0[-2]) );
#endif
#if !JVET_N0242_NON_LINEAR_ALF
            sum += filterCoeff[11] * ( pImg0[+1] + pImg0[-1] );
#else
            sum += filterCoeff[11] * ( clipALF(filterClipp[11], curr, pImg0[+1], pImg0[-1]) );
#endif
#if !JVET_N0242_NON_LINEAR_ALF
            sum += filterCoeff[12] * ( pImg0[+0] );
#endif
          }
          else
          {
#if !JVET_N0242_NON_LINEAR_ALF
            sum += filterCoeff[0] * ( pImg3[+0] + pImg4[+0] );
#else
            sum += filterCoeff[0] * ( clipALF(filterClipp[0], curr, pImg3[+0], pImg4[+0]) );
#endif

#if !JVET_N0242_NON_LINEAR_ALF
            sum += filterCoeff[1] * ( pImg1[+1] + pImg2[-1] );
#else
            sum += filterCoeff[1] * ( clipALF(filterClipp[1], curr, pImg1[+1], pImg2[-1]) );
#endif
#if !JVET_N0242_NON_LINEAR_ALF
            sum += filterCoeff[2] * ( pImg1[+0] + pImg2[+0] );
#else
            sum += filterCoeff[2] * ( clipALF(filterClipp[2], curr, pImg1[+0], pImg2[+0]) );
#endif
#if !JVET_N0242_NON_LINEAR_ALF
            sum += filterCoeff[3] * ( pImg1[-1] + pImg2[+1] );
#else
            sum += filterCoeff[3] * ( clipALF(filterClipp[3], curr, pImg1[-1], pImg2[+1]) );
#endif

#if !JVET_N0242_NON_LINEAR_ALF
            sum += filterCoeff[4] * ( pImg0[+2] + pImg0[-2] );
#else
            sum += filterCoeff[4] * ( clipALF(filterClipp[4], curr, pImg0[+2], pImg0[-2]) );
#endif
#if !JVET_N0242_NON_LINEAR_ALF
            sum += filterCoeff[5] * ( pImg0[+1] + pImg0[-1] );
#else
            sum += filterCoeff[5] * ( clipALF(filterClipp[5], curr, pImg0[+1], pImg0[-1]) );
#endif
#if !JVET_N0242_NON_LINEAR_ALF
            sum += filterCoeff[6] * ( pImg0[+0] );
#endif
          }

          sum = ( sum + offset ) >> shift;
#if JVET_N0242_NON_LINEAR_ALF
          sum += curr;
#endif
          pRec1[jj] = ClipPel( sum, clpRng );

          pImg0++;
          pImg1++;
          pImg2++;
          pImg3++;
          pImg4++;
          pImg5++;
          pImg6++;
        }
      }
    }

    pRec0 += dstStride2;
    pRec1 += dstStride2;

    pImgYPad0 += srcStride2;
    pImgYPad1 += srcStride2;
    pImgYPad2 += srcStride2;
    pImgYPad3 += srcStride2;
    pImgYPad4 += srcStride2;
    pImgYPad5 += srcStride2;
    pImgYPad6 += srcStride2;
  }
}
