// Copyright (c) 2026 F4TNK
// SPDX-License-Identifier: BSL-1.0
//
// SSE2/SIMD-optimized sample format converters for SoapySDR.
// These register at VECTORIZED priority (above GENERIC) and handle
// the most performance-critical conversion paths in SDR reception:
//   - CS16 <-> CF32  (AirSpy, RTL-SDR via osmosdr)
//   - CU8  <-> CF32  (RTL-SDR native)
//   - CS8  <-> CF32  (HackRF, etc.)
//
// On x86-64, SSE2 is always available (part of the architecture).
// On other architectures, these fall back to optimized scalar with
// __restrict__ hints for auto-vectorization.

#include <SoapySDR/ConverterRegistry.hpp>
#include <SoapySDR/Formats.hpp>
#include <cstring>
#include <cstdint>

// Detect SSE2 availability
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
    #define SOAPY_USE_SSE2 1
    #include <emmintrin.h>
#else
    #define SOAPY_USE_SSE2 0
#endif

// Detect AVX2 availability (Intel Haswell+ / AMD Ryzen+)
// -march=native automatically defines __AVX2__ when supported by the host CPU.
#if defined(__AVX2__)
    #define SOAPY_USE_AVX2 1
    #include <immintrin.h>
#else
    #define SOAPY_USE_AVX2 0
#endif

// Restrict hint for auto-vectorization
#if defined(__GNUC__) || defined(__clang__)
    #define SOAPY_RESTRICT __restrict__
#else
    #define SOAPY_RESTRICT
#endif

//=============================================================================
// CS16 -> CF32 (SIMD optimized)
// Used when: application requests float from int16 driver
//=============================================================================
static void simdCS16toCF32(const void *srcBuff, void *dstBuff, const size_t numElems, const double scaler)
{
    const int16_t * SOAPY_RESTRICT src = (const int16_t *)srcBuff;
    float * SOAPY_RESTRICT dst = (float *)dstBuff;
    const size_t total = numElems * 2;  // I + Q per element
    const float fScaler = float(scaler / 32768.0);

#if SOAPY_USE_SSE2
    const __m128 vScaler = _mm_set1_ps(fScaler);
    size_t i = 0;

#if SOAPY_USE_AVX2
    // Mod 20 — AVX2: 8 complex samples (16 int16) per iteration — ×2 vs SSE2
    const __m256 vScaler256 = _mm256_set1_ps(fScaler);
    for (; i + 15 < total; i += 16)
    {
        _mm_prefetch((const char*)(src + i + 256), _MM_HINT_T1);
        __m256i vi16 = _mm256_loadu_si256((const __m256i*)(src + i));
        // sign-extend lower/upper 8 int16 -> 8 int32 (AVX2 one-shot)
        __m256i lo32 = _mm256_cvtepi16_epi32(_mm256_castsi256_si128(vi16));
        __m256i hi32 = _mm256_cvtepi16_epi32(_mm256_extracti128_si256(vi16, 1));
        __m256 flo = _mm256_mul_ps(_mm256_cvtepi32_ps(lo32), vScaler256);
        __m256 fhi = _mm256_mul_ps(_mm256_cvtepi32_ps(hi32), vScaler256);
        _mm256_storeu_ps(dst + i,     flo);
        _mm256_storeu_ps(dst + i + 8, fhi);
    }
#endif

    // SSE2 tail for remaining < 16 elements
    for (; i + 7 < total; i += 8)
    {
        _mm_prefetch((const char*)(src + i + 128), _MM_HINT_T0);
        __m128i vi16 = _mm_loadu_si128((const __m128i *)(src + i));
        __m128i lo32 = _mm_srai_epi32(_mm_unpacklo_epi16(vi16, vi16), 16);
        __m128i hi32 = _mm_srai_epi32(_mm_unpackhi_epi16(vi16, vi16), 16);
        __m128 flo = _mm_mul_ps(_mm_cvtepi32_ps(lo32), vScaler);
        __m128 fhi = _mm_mul_ps(_mm_cvtepi32_ps(hi32), vScaler);
        _mm_storeu_ps(dst + i,     flo);
        _mm_storeu_ps(dst + i + 4, fhi);
    }

    // Scalar tail
    for (; i < total; i++)
        dst[i] = float(src[i]) * fScaler;

#else
    for (size_t i = 0; i < total; i++)
        dst[i] = float(src[i]) * fScaler;
#endif
}

//=============================================================================
// CF32 -> CS16 (SIMD optimized)
// Used when: driver needs int16 from float application
//=============================================================================
static void simdCF32toCS16(const void *srcBuff, void *dstBuff, const size_t numElems, const double scaler)
{
    const float * SOAPY_RESTRICT src = (const float *)srcBuff;
    int16_t * SOAPY_RESTRICT dst = (int16_t *)dstBuff;
    const size_t total = numElems * 2;
    const float fScaler = float(scaler * 32768.0);

#if SOAPY_USE_SSE2
    const __m128 vScaler = _mm_set1_ps(fScaler);
    size_t i = 0;

#if SOAPY_USE_AVX2
    // Mod 21 — AVX2: 8 complex samples (16 floats in / 16 int16 out) per iteration
    const __m256 vScaler256 = _mm256_set1_ps(fScaler);
    for (; i + 15 < total; i += 16)
    {
        _mm_prefetch((const char*)(src + i + 64), _MM_HINT_T1);
        __m256 fa = _mm256_loadu_ps(src + i);
        __m256 fb = _mm256_loadu_ps(src + i + 8);
        fa = _mm256_mul_ps(fa, vScaler256);
        fb = _mm256_mul_ps(fb, vScaler256);
        __m256i ia = _mm256_cvtps_epi32(fa);
        __m256i ib = _mm256_cvtps_epi32(fb);
        // packs_epi32 works within each 128-bit lane; permute to restore sequential order
        __m256i packed = _mm256_packs_epi32(ia, ib);
        packed = _mm256_permute4x64_epi64(packed, 0xD8); // [0,2,1,3]
        _mm256_storeu_si256((__m256i*)(dst + i), packed);
    }
#endif

    for (; i + 7 < total; i += 8)
    {
        _mm_prefetch((const char*)(src + i + 64), _MM_HINT_T0);
        __m128 flo = _mm_loadu_ps(src + i);
        __m128 fhi = _mm_loadu_ps(src + i + 4);
        flo = _mm_mul_ps(flo, vScaler);
        fhi = _mm_mul_ps(fhi, vScaler);
        __m128i ilo = _mm_cvtps_epi32(flo);
        __m128i ihi = _mm_cvtps_epi32(fhi);
        __m128i packed = _mm_packs_epi32(ilo, ihi);
        _mm_storeu_si128((__m128i *)(dst + i), packed);
    }

    for (; i < total; i++)
    {
        float v = src[i] * fScaler;
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        dst[i] = (int16_t)v;
    }
#else
    for (size_t i = 0; i < total; i++)
    {
        float v = src[i] * fScaler;
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        dst[i] = (int16_t)v;
    }
#endif
}

//=============================================================================
// CU8 -> CF32 (SIMD optimized)
// Used when: RTL-SDR native uint8 needs float conversion
//=============================================================================
static void simdCU8toCF32(const void *srcBuff, void *dstBuff, const size_t numElems, const double scaler)
{
    const uint8_t * SOAPY_RESTRICT src = (const uint8_t *)srcBuff;
    float * SOAPY_RESTRICT dst = (float *)dstBuff;
    const size_t total = numElems * 2;
    const float fScaler = float(scaler / 128.0);
    const float fOffset = 128.0f;

#if SOAPY_USE_SSE2
    const __m128 vScaler = _mm_set1_ps(fScaler);
    const __m128 vOffset = _mm_set1_ps(fOffset);
    const __m128i zero = _mm_setzero_si128();
    size_t i = 0;

#if SOAPY_USE_AVX2
    // Mod 22 — AVX2: 8 complex samples (16 bytes in / 16 floats out) per iteration
    const __m256 vScaler256 = _mm256_set1_ps(fScaler);
    const __m256 vOffset256 = _mm256_set1_ps(fOffset);
    for (; i + 15 < total; i += 16)
    {
        _mm_prefetch((const char*)(src + i + 512), _MM_HINT_T1);
        // Load 16 uint8, zero-extend to 16 uint16 in 256 bits (AVX2 one-shot)
        __m128i vu8 = _mm_loadu_si128((const __m128i*)(src + i));
        __m256i vu16 = _mm256_cvtepu8_epi16(vu8);
        // Zero-extend lower/upper 8 uint16 -> 8 int32
        __m256i lo32 = _mm256_cvtepi16_epi32(_mm256_castsi256_si128(vu16));
        __m256i hi32 = _mm256_cvtepi16_epi32(_mm256_extracti128_si256(vu16, 1));
        __m256 flo = _mm256_mul_ps(_mm256_sub_ps(_mm256_cvtepi32_ps(lo32), vOffset256), vScaler256);
        __m256 fhi = _mm256_mul_ps(_mm256_sub_ps(_mm256_cvtepi32_ps(hi32), vOffset256), vScaler256);
        _mm256_storeu_ps(dst + i,     flo);
        _mm256_storeu_ps(dst + i + 8, fhi);
    }
#endif

    for (; i + 7 < total; i += 8)
    {
        _mm_prefetch((const char*)(src + i + 256), _MM_HINT_T0);
        __m128i vu8 = _mm_loadl_epi64((const __m128i *)(src + i));
        __m128i vu16 = _mm_unpacklo_epi8(vu8, zero);
        __m128i lo32 = _mm_unpacklo_epi16(vu16, zero);
        __m128i hi32 = _mm_unpackhi_epi16(vu16, zero);
        __m128 flo = _mm_mul_ps(_mm_sub_ps(_mm_cvtepi32_ps(lo32), vOffset), vScaler);
        __m128 fhi = _mm_mul_ps(_mm_sub_ps(_mm_cvtepi32_ps(hi32), vOffset), vScaler);
        _mm_storeu_ps(dst + i,     flo);
        _mm_storeu_ps(dst + i + 4, fhi);
    }

    for (; i < total; i++)
        dst[i] = (float(src[i]) - fOffset) * fScaler;

#else
    for (size_t i = 0; i < total; i++)
        dst[i] = (float(src[i]) - fOffset) * fScaler;
#endif
}

//=============================================================================
// CF32 -> CU8 (SIMD optimized)
//=============================================================================
static void simdCF32toCU8(const void *srcBuff, void *dstBuff, const size_t numElems, const double scaler)
{
    const float * SOAPY_RESTRICT src = (const float *)srcBuff;
    uint8_t * SOAPY_RESTRICT dst = (uint8_t *)dstBuff;
    const size_t total = numElems * 2;
    const float fScaler = float(scaler * 128.0);
    const float fOffset = 128.0f;

#if SOAPY_USE_SSE2
    const __m128 vScaler = _mm_set1_ps(fScaler);
    const __m128 vOffset = _mm_set1_ps(fOffset);
    const __m128 vMin = _mm_set1_ps(0.0f);
    const __m128 vMax = _mm_set1_ps(255.0f);
    size_t i = 0;

    for (; i + 7 < total; i += 8)
    {
        _mm_prefetch((const char*)(src + i + 64), _MM_HINT_T0);

        // Load 8 floats
        __m128 flo = _mm_loadu_ps(src + i);
        __m128 fhi = _mm_loadu_ps(src + i + 4);

        // Scale and add offset: val = src * 128 + 128
        flo = _mm_add_ps(_mm_mul_ps(flo, vScaler), vOffset);
        fhi = _mm_add_ps(_mm_mul_ps(fhi, vScaler), vOffset);

        // Clamp to [0, 255]
        flo = _mm_max_ps(_mm_min_ps(flo, vMax), vMin);
        fhi = _mm_max_ps(_mm_min_ps(fhi, vMax), vMin);

        // Convert to int32
        __m128i ilo = _mm_cvtps_epi32(flo);
        __m128i ihi = _mm_cvtps_epi32(fhi);

        // Pack int32 -> int16 (signed saturation OK since values are 0-255)
        __m128i i16 = _mm_packs_epi32(ilo, ihi);
        // Pack int16 -> uint8 (unsigned saturation)
        __m128i i8 = _mm_packus_epi16(i16, i16);

        // Store lower 8 bytes
        _mm_storel_epi64((__m128i *)(dst + i), i8);
    }

    // Scalar tail
    for (; i < total; i++)
    {
        float v = src[i] * fScaler + fOffset;
        if (v > 255.0f) v = 255.0f;
        if (v < 0.0f) v = 0.0f;
        dst[i] = (uint8_t)v;
    }
#else
    for (size_t i = 0; i < total; i++)
    {
        float v = src[i] * fScaler + fOffset;
        if (v > 255.0f) v = 255.0f;
        if (v < 0.0f) v = 0.0f;
        dst[i] = (uint8_t)v;
    }
#endif
}

//=============================================================================
// CS8 -> CF32 (SIMD optimized)
// Used when: HackRF int8 needs float conversion
//=============================================================================
static void simdCS8toCF32(const void *srcBuff, void *dstBuff, const size_t numElems, const double scaler)
{
    const int8_t * SOAPY_RESTRICT src = (const int8_t *)srcBuff;
    float * SOAPY_RESTRICT dst = (float *)dstBuff;
    const size_t total = numElems * 2;
    const float fScaler = float(scaler / 128.0);

#if SOAPY_USE_SSE2
    const __m128 vScaler = _mm_set1_ps(fScaler);
    const __m128i zero = _mm_setzero_si128();
    size_t i = 0;

#if SOAPY_USE_AVX2
    // Mod 23 — AVX2: 8 complex samples (16 int8 in / 16 floats out) per iteration
    const __m256 vScaler256 = _mm256_set1_ps(fScaler);
    for (; i + 15 < total; i += 16)
    {
        _mm_prefetch((const char*)(src + i + 256), _MM_HINT_T1);
        // Load 16 int8, sign-extend to 16 int16 in 256 bits (AVX2 one-shot)
        __m128i vi8 = _mm_loadu_si128((const __m128i*)(src + i));
        __m256i vi16 = _mm256_cvtepi8_epi16(vi8);
        // sign-extend lower/upper 8 int16 -> 8 int32
        __m256i lo32 = _mm256_cvtepi16_epi32(_mm256_castsi256_si128(vi16));
        __m256i hi32 = _mm256_cvtepi16_epi32(_mm256_extracti128_si256(vi16, 1));
        __m256 flo = _mm256_mul_ps(_mm256_cvtepi32_ps(lo32), vScaler256);
        __m256 fhi = _mm256_mul_ps(_mm256_cvtepi32_ps(hi32), vScaler256);
        _mm256_storeu_ps(dst + i,     flo);
        _mm256_storeu_ps(dst + i + 8, fhi);
    }
#endif

    for (; i + 7 < total; i += 8)
    {
        _mm_prefetch((const char*)(src + i + 256), _MM_HINT_T0);
        __m128i vi8 = _mm_loadl_epi64((const __m128i *)(src + i));
        __m128i sign = _mm_cmpgt_epi8(zero, vi8);
        __m128i vi16 = _mm_unpacklo_epi8(vi8, sign);
        __m128i signw = _mm_srai_epi16(vi16, 15);
        __m128i lo32 = _mm_unpacklo_epi16(vi16, signw);
        __m128i hi32 = _mm_unpackhi_epi16(vi16, signw);
        __m128 flo = _mm_mul_ps(_mm_cvtepi32_ps(lo32), vScaler);
        __m128 fhi = _mm_mul_ps(_mm_cvtepi32_ps(hi32), vScaler);
        _mm_storeu_ps(dst + i,     flo);
        _mm_storeu_ps(dst + i + 4, fhi);
    }

    for (; i < total; i++)
        dst[i] = float(src[i]) * fScaler;

#else
    for (size_t i = 0; i < total; i++)
        dst[i] = float(src[i]) * fScaler;
#endif
}

//=============================================================================
// CF32 -> CS8 (SIMD optimized)
//=============================================================================
static void simdCF32toCS8(const void *srcBuff, void *dstBuff, const size_t numElems, const double scaler)
{
    const float * SOAPY_RESTRICT src = (const float *)srcBuff;
    int8_t * SOAPY_RESTRICT dst = (int8_t *)dstBuff;
    const size_t total = numElems * 2;
    const float fScaler = float(scaler * 128.0);

#if SOAPY_USE_SSE2
    const __m128 vScaler = _mm_set1_ps(fScaler);
    size_t i = 0;

    for (; i + 7 < total; i += 8)
    {
        _mm_prefetch((const char*)(src + i + 64), _MM_HINT_T0);

        __m128 flo = _mm_loadu_ps(src + i);
        __m128 fhi = _mm_loadu_ps(src + i + 4);

        flo = _mm_mul_ps(flo, vScaler);
        fhi = _mm_mul_ps(fhi, vScaler);

        __m128i ilo = _mm_cvtps_epi32(flo);
        __m128i ihi = _mm_cvtps_epi32(fhi);

        // Pack int32 -> int16 with saturation
        __m128i i16 = _mm_packs_epi32(ilo, ihi);
        // Pack int16 -> int8 with saturation
        __m128i i8 = _mm_packs_epi16(i16, i16);

        // Store lower 8 bytes
        _mm_storel_epi64((__m128i *)(dst + i), i8);
    }

    for (; i < total; i++)
    {
        float v = src[i] * fScaler;
        if (v > 127.0f) v = 127.0f;
        if (v < -128.0f) v = -128.0f;
        dst[i] = (int8_t)v;
    }
#else
    for (size_t i = 0; i < total; i++)
    {
        float v = src[i] * fScaler;
        if (v > 127.0f) v = 127.0f;
        if (v < -128.0f) v = -128.0f;
        dst[i] = (int8_t)v;
    }
#endif
}

//=============================================================================
// CF32 -> CF32 (optimized memcpy fast-path)
//=============================================================================
static void simdCF32toCF32(const void *srcBuff, void *dstBuff, const size_t numElems, const double scaler)
{
    if (scaler == 1.0)
    {
        std::memcpy(dstBuff, srcBuff, numElems * 2 * sizeof(float));
        return;
    }

    const float * SOAPY_RESTRICT src = (const float *)srcBuff;
    float * SOAPY_RESTRICT dst = (float *)dstBuff;
    const size_t total = numElems * 2;
    const float fScaler = float(scaler);

#if SOAPY_USE_SSE2
    const __m128 vScaler = _mm_set1_ps(fScaler);
    size_t i = 0;

    for (; i + 3 < total; i += 4)
    {
        _mm_prefetch((const char*)(src + i + 64), _MM_HINT_T0);
        __m128 v = _mm_loadu_ps(src + i);
        v = _mm_mul_ps(v, vScaler);
        _mm_storeu_ps(dst + i, v);
    }

    for (; i < total; i++)
    {
        dst[i] = src[i] * fScaler;
    }
#else
    for (size_t i = 0; i < total; i++)
    {
        dst[i] = src[i] * fScaler;
    }
#endif
}

//=============================================================================
// CS16 -> CS16 (optimized memcpy + SIMD scale)
//=============================================================================
static void simdCS16toCS16(const void *srcBuff, void *dstBuff, const size_t numElems, const double scaler)
{
    if (scaler == 1.0)
    {
        std::memcpy(dstBuff, srcBuff, numElems * 2 * sizeof(int16_t));
        return;
    }

    const int16_t * SOAPY_RESTRICT src = (const int16_t *)srcBuff;
    int16_t * SOAPY_RESTRICT dst = (int16_t *)dstBuff;
    const size_t total = numElems * 2;

    // Scale via float conversion for accuracy
    const float fScaler = float(scaler);

#if SOAPY_USE_SSE2
    const __m128 vScaler = _mm_set1_ps(fScaler);
    size_t i = 0;

    for (; i + 7 < total; i += 8)
    {
        _mm_prefetch((const char*)(src + i + 128), _MM_HINT_T0);

        __m128i vi16 = _mm_loadu_si128((const __m128i *)(src + i));

        // Sign-extend to int32 and convert to float
        __m128i lo32 = _mm_srai_epi32(_mm_unpacklo_epi16(vi16, vi16), 16);
        __m128i hi32 = _mm_srai_epi32(_mm_unpackhi_epi16(vi16, vi16), 16);

        __m128 flo = _mm_mul_ps(_mm_cvtepi32_ps(lo32), vScaler);
        __m128 fhi = _mm_mul_ps(_mm_cvtepi32_ps(hi32), vScaler);

        __m128i ilo = _mm_cvtps_epi32(flo);
        __m128i ihi = _mm_cvtps_epi32(fhi);

        __m128i packed = _mm_packs_epi32(ilo, ihi);
        _mm_storeu_si128((__m128i *)(dst + i), packed);
    }

    for (; i < total; i++)
    {
        dst[i] = (int16_t)(float(src[i]) * fScaler);
    }
#else
    for (size_t i = 0; i < total; i++)
    {
        dst[i] = (int16_t)(float(src[i]) * fScaler);
    }
#endif
}

//=============================================================================
// CU16 -> CF32 (SIMD optimized)
// Used when: SDRPlay / some RTL-SDR modes output unsigned 16-bit
//=============================================================================
static void simdCU16toCF32(const void *srcBuff, void *dstBuff, const size_t numElems, const double scaler)
{
    const uint16_t * SOAPY_RESTRICT src = (const uint16_t *)srcBuff;
    float * SOAPY_RESTRICT dst = (float *)dstBuff;
    const size_t total = numElems * 2;
    const float fScaler = float(scaler / 32768.0);
    const float fOffset = 32768.0f;

#if SOAPY_USE_SSE2
    const __m128 vScaler = _mm_set1_ps(fScaler);
    const __m128 vOffset = _mm_set1_ps(fOffset);
    const __m128i zero = _mm_setzero_si128();
    size_t i = 0;

    for (; i + 7 < total; i += 8)
    {
        _mm_prefetch((const char*)(src + i + 128), _MM_HINT_T0);

        // Load 8 x uint16
        __m128i vu16 = _mm_loadu_si128((const __m128i *)(src + i));

        // Zero-extend to int32
        __m128i lo32 = _mm_unpacklo_epi16(vu16, zero);
        __m128i hi32 = _mm_unpackhi_epi16(vu16, zero);

        // Convert to float, subtract offset, scale
        __m128 flo = _mm_mul_ps(_mm_sub_ps(_mm_cvtepi32_ps(lo32), vOffset), vScaler);
        __m128 fhi = _mm_mul_ps(_mm_sub_ps(_mm_cvtepi32_ps(hi32), vOffset), vScaler);

        _mm_storeu_ps(dst + i, flo);
        _mm_storeu_ps(dst + i + 4, fhi);
    }

    for (; i < total; i++)
    {
        dst[i] = (float(src[i]) - fOffset) * fScaler;
    }
#else
    for (size_t i = 0; i < total; i++)
    {
        dst[i] = (float(src[i]) - fOffset) * fScaler;
    }
#endif
}

//=============================================================================
// CF32 -> CU16 (SIMD optimized)
//=============================================================================
static void simdCF32toCU16(const void *srcBuff, void *dstBuff, const size_t numElems, const double scaler)
{
    const float * SOAPY_RESTRICT src = (const float *)srcBuff;
    uint16_t * SOAPY_RESTRICT dst = (uint16_t *)dstBuff;
    const size_t total = numElems * 2;
    const float fScaler = float(scaler * 32768.0);
    const float fOffset = 32768.0f;

#if SOAPY_USE_SSE2
    const __m128 vScaler = _mm_set1_ps(fScaler);
    const __m128 vOffset = _mm_set1_ps(fOffset);
    const __m128 vMin = _mm_set1_ps(0.0f);
    const __m128 vMax = _mm_set1_ps(65535.0f);
    const __m128i vBias32 = _mm_set1_epi32(32768);
    const __m128i vBias16 = _mm_set1_epi16(-32768);  // 0x8000 for sign flip
    size_t i = 0;

    for (; i + 7 < total; i += 8)
    {
        _mm_prefetch((const char*)(src + i + 64), _MM_HINT_T0);

        __m128 flo = _mm_loadu_ps(src + i);
        __m128 fhi = _mm_loadu_ps(src + i + 4);

        // Scale and add offset
        flo = _mm_add_ps(_mm_mul_ps(flo, vScaler), vOffset);
        fhi = _mm_add_ps(_mm_mul_ps(fhi, vScaler), vOffset);

        // Clamp to [0, 65535]
        flo = _mm_max_ps(_mm_min_ps(flo, vMax), vMin);
        fhi = _mm_max_ps(_mm_min_ps(fhi, vMax), vMin);

        // Convert to int32
        __m128i ilo = _mm_cvtps_epi32(flo);
        __m128i ihi = _mm_cvtps_epi32(fhi);

        // SSE2: shift to signed range, pack int32->int16, flip sign back
        __m128i ilo_s = _mm_sub_epi32(ilo, vBias32);
        __m128i ihi_s = _mm_sub_epi32(ihi, vBias32);
        __m128i packed_signed = _mm_packs_epi32(ilo_s, ihi_s);
        __m128i packed = _mm_xor_si128(packed_signed, vBias16);

        _mm_storeu_si128((__m128i *)(dst + i), packed);
    }

    for (; i < total; i++)
    {
        float v = src[i] * fScaler + fOffset;
        if (v > 65535.0f) v = 65535.0f;
        if (v < 0.0f) v = 0.0f;
        dst[i] = (uint16_t)v;
    }
#else
    for (size_t i = 0; i < total; i++)
    {
        float v = src[i] * fScaler + fOffset;
        if (v > 65535.0f) v = 65535.0f;
        if (v < 0.0f) v = 0.0f;
        dst[i] = (uint16_t)v;
    }
#endif
}

//=============================================================================
// CU8 -> CS16 (SIMD optimized — direct integer path, no float intermediate)
// Used when: RTL-SDR CU8 needs int16 output without CF32 conversion step
// Processes 16 bytes (8 complex samples) per iteration
//=============================================================================
static void simdCU8toCS16(const void *srcBuff, void *dstBuff, const size_t numElems, const double scaler)
{
    const uint8_t * SOAPY_RESTRICT src = (const uint8_t *)srcBuff;
    int16_t * SOAPY_RESTRICT dst = (int16_t *)dstBuff;
    const size_t total = numElems * 2;

#if SOAPY_USE_SSE2
    if (scaler == 1.0)
    {
        const __m128i vBias8 = _mm_set1_epi8((char)0x80);  // for unsigned→signed
        const __m128i zero = _mm_setzero_si128();
        size_t i = 0;

        for (; i + 15 < total; i += 16)
        {
            _mm_prefetch((const char*)(src + i + 256), _MM_HINT_T0);

            // Load 16 uint8 (8 complex samples)
            __m128i vu8 = _mm_loadu_si128((const __m128i *)(src + i));

            // Convert unsigned to signed by XOR with 0x80
            __m128i vs8 = _mm_xor_si128(vu8, vBias8);

            // Sign-extend int8 → int16
            __m128i sign = _mm_cmpgt_epi8(zero, vs8);
            __m128i lo16 = _mm_unpacklo_epi8(vs8, sign);
            __m128i hi16 = _mm_unpackhi_epi8(vs8, sign);

            // Shift left by 8 to map to CS16 full scale [-32768, 32512]
            lo16 = _mm_slli_epi16(lo16, 8);
            hi16 = _mm_slli_epi16(hi16, 8);

            _mm_storeu_si128((__m128i *)(dst + i), lo16);
            _mm_storeu_si128((__m128i *)(dst + i + 8), hi16);
        }

        for (; i < total; i++)
        {
            dst[i] = (int16_t)((int(src[i]) - 128) << 8);
        }
    }
    else
#endif
    {
        const double fullScale = scaler * 256.0;
        for (size_t i = 0; i < total; i++)
        {
            dst[i] = (int16_t)((int(src[i]) - 128) * fullScale);
        }
    }
}

//=============================================================================
// CS16 -> CU8 (SIMD optimized — direct integer path)
// Processes 16 int16 (8 complex samples) per iteration
//=============================================================================
static void simdCS16toCU8(const void *srcBuff, void *dstBuff, const size_t numElems, const double scaler)
{
    const int16_t * SOAPY_RESTRICT src = (const int16_t *)srcBuff;
    uint8_t * SOAPY_RESTRICT dst = (uint8_t *)dstBuff;
    const size_t total = numElems * 2;

#if SOAPY_USE_SSE2
    if (scaler == 1.0)
    {
        const __m128i vBias8 = _mm_set1_epi8((char)0x80);
        size_t i = 0;

        for (; i + 15 < total; i += 16)
        {
            _mm_prefetch((const char*)(src + i + 128), _MM_HINT_T0);

            // Load 16 int16 (8 complex samples)
            __m128i lo16 = _mm_loadu_si128((const __m128i *)(src + i));
            __m128i hi16 = _mm_loadu_si128((const __m128i *)(src + i + 8));

            // Arithmetic right shift by 8: int16 [-32768,32767] → int8 [-128,127]
            lo16 = _mm_srai_epi16(lo16, 8);
            hi16 = _mm_srai_epi16(hi16, 8);

            // Pack int16 → int8 with signed saturation
            __m128i vs8 = _mm_packs_epi16(lo16, hi16);

            // Convert signed to unsigned: XOR with 0x80
            __m128i vu8 = _mm_xor_si128(vs8, vBias8);

            _mm_storeu_si128((__m128i *)(dst + i), vu8);
        }

        for (; i < total; i++)
        {
            dst[i] = (uint8_t)((src[i] >> 8) + 128);
        }
    }
    else
#endif
    {
        for (size_t i = 0; i < total; i++)
        {
            int v = (int)(src[i] * scaler) >> 8;
            if (v > 127) v = 127;
            if (v < -128) v = -128;
            dst[i] = (uint8_t)(v + 128);
        }
    }
}

//=============================================================================
// Register SIMD converters at VECTORIZED priority
// These override the GENERIC scalar converters when available
//=============================================================================

// Use a function called by lateLoadDefaultConverters to ensure proper ordering
void lateLoadSIMDConverters(void)
{
    // Complex format converters (hot paths for SDR)
    static SoapySDR::ConverterRegistry regCS16toCF32(SOAPY_SDR_CS16, SOAPY_SDR_CF32, SoapySDR::ConverterRegistry::VECTORIZED, &simdCS16toCF32);
    static SoapySDR::ConverterRegistry regCF32toCS16(SOAPY_SDR_CF32, SOAPY_SDR_CS16, SoapySDR::ConverterRegistry::VECTORIZED, &simdCF32toCS16);
    static SoapySDR::ConverterRegistry regCU8toCF32(SOAPY_SDR_CU8, SOAPY_SDR_CF32, SoapySDR::ConverterRegistry::VECTORIZED, &simdCU8toCF32);
    static SoapySDR::ConverterRegistry regCF32toCU8(SOAPY_SDR_CF32, SOAPY_SDR_CU8, SoapySDR::ConverterRegistry::VECTORIZED, &simdCF32toCU8);
    static SoapySDR::ConverterRegistry regCS8toCF32(SOAPY_SDR_CS8, SOAPY_SDR_CF32, SoapySDR::ConverterRegistry::VECTORIZED, &simdCS8toCF32);
    static SoapySDR::ConverterRegistry regCF32toCS8(SOAPY_SDR_CF32, SOAPY_SDR_CS8, SoapySDR::ConverterRegistry::VECTORIZED, &simdCF32toCS8);

    // Unsigned 16-bit converters (SDRPlay, etc.)
    static SoapySDR::ConverterRegistry regCU16toCF32(SOAPY_SDR_CU16, SOAPY_SDR_CF32, SoapySDR::ConverterRegistry::VECTORIZED, &simdCU16toCF32);
    static SoapySDR::ConverterRegistry regCF32toCU16(SOAPY_SDR_CF32, SOAPY_SDR_CU16, SoapySDR::ConverterRegistry::VECTORIZED, &simdCF32toCU16);

    // Direct integer-to-integer paths (avoid float intermediate)
    static SoapySDR::ConverterRegistry regCU8toCS16(SOAPY_SDR_CU8, SOAPY_SDR_CS16, SoapySDR::ConverterRegistry::VECTORIZED, &simdCU8toCS16);
    static SoapySDR::ConverterRegistry regCS16toCU8(SOAPY_SDR_CS16, SOAPY_SDR_CU8, SoapySDR::ConverterRegistry::VECTORIZED, &simdCS16toCU8);

    // Same-format optimized copies
    static SoapySDR::ConverterRegistry regCF32toCF32(SOAPY_SDR_CF32, SOAPY_SDR_CF32, SoapySDR::ConverterRegistry::VECTORIZED, &simdCF32toCF32);
    static SoapySDR::ConverterRegistry regCS16toCS16(SOAPY_SDR_CS16, SOAPY_SDR_CS16, SoapySDR::ConverterRegistry::VECTORIZED, &simdCS16toCS16);
}
