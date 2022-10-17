// Copyright 2019 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Unittest for the TCMalloc implementation.
//
// * The test consists of a set of threads.
// * Each thread maintains a set of allocated objects, with
//   a bound on the total amount of data in the set.
// * Each allocated object's contents are generated by
//   hashing the object pointer, and a generation count
//   in the object.  This allows us to easily check for
//   data corruption.
// * At any given step, the thread can do any of the following:
//     a. Allocate an object
//     b. Increment an object's generation count and update
//        its contents.
//     c. Pass the object to another thread
//     d. Free an object
//   Also, at the end of every step, object(s) are freed to maintain
//   the memory upper-bound.

#define _XOPEN_SOURCE 600
#define _GNU_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <random>
#include <string>
#include <thread>  // NOLINT(build/c++11)
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "benchmark/benchmark.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/casts.h"
#include "absl/base/macros.h"
#include "absl/base/optimization.h"
#include "absl/container/flat_hash_set.h"
#include "absl/numeric/bits.h"
#include "absl/random/random.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/declarations.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/parameter_accessors.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/new_extension.h"
#include "tcmalloc/testing/test_allocator_harness.h"
#include "tcmalloc/testing/testutil.h"
#include "tcmalloc/testing/thread_manager.h"

// Windows doesn't define pvalloc and a few other obsolete unix
// functions; nor does it define posix_memalign (which is not obsolete).
#if defined(_WIN32)
#define cfree free
#define valloc malloc
#define pvalloc malloc
static inline int PosixMemalign(void** ptr, size_t align, size_t size) {
  tcmalloc::Crash(tcmalloc::kCrash, __FILE__, __LINE__,
                  "posix_memalign not supported on windows");
}

// OS X defines posix_memalign in some OS versions but not others;
// it's confusing enough to check that it's easiest to just not to test.
#elif defined(__APPLE__)
static inline int PosixMemalign(void** ptr, size_t align, size_t size) {
  tcmalloc::Crash(tcmalloc::kCrash, __FILE__, __LINE__,
                  "posix_memalign not supported on OS X");
}

#else
#define OS_SUPPORTS_MEMALIGN
static inline int PosixMemalign(void** ptr, size_t align, size_t size) {
  return posix_memalign(ptr, align, size);
}

#endif

// Testing parameters
//
// When making aligned allocations, we pick a power of two up to 1 <<
// kLogMaxMemalign.
const int kLogMaxMemalign = 18;

#if !defined(__STDC_VERSION_STDLIB_H__) || __STDC_VERSION_STDLIB_H__ < 202311L
// free_sized is a sized free function introduced in C23.
extern "C" void free_sized(void* ptr, size_t size) noexcept;
// free_aligned_sized is an overaligned sized free function introduced in C23.
extern "C" void free_aligned_sized(void* ptr, size_t align,
                                   size_t size) noexcept;
#endif

#if !defined(__GLIBC__)
extern "C" int malloc_info(int opt, FILE* fp) noexcept;
#endif

static const int kSizeBits = 8 * sizeof(size_t);
static const size_t kMaxTestSize = ~static_cast<size_t>(0);
static const size_t kMaxSignedSize = ((size_t(1) << (kSizeBits - 1)) - 1);

namespace tcmalloc {
extern ABSL_ATTRIBUTE_WEAK bool want_hpaa();
}  // namespace tcmalloc

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  SetTestResourceLimit();

  return RUN_ALL_TESTS();
}

namespace tcmalloc {
namespace {

TEST(TcmallocTest, EmptyAllocations) {
  // Check that empty allocation works
  void* p1 = ::operator new(0);
  ASSERT_NE(p1, nullptr);
  void* p2 = ::operator new(0);
  ASSERT_NE(p2, nullptr);
  ASSERT_NE(p1, p2);
  ::operator delete(p1);
  ::operator delete(p2);
}

TEST(TcmallocTest, LargeAllocation) {
  // Check that "lots" of memory can be allocated
  constexpr size_t kMB = 1 << 20;
  ::operator delete(::operator new(100 * kMB));
}

TEST(TcmallocTest, Calloc) {
  // Check calloc() with various arguments

  struct TestCase {
    size_t n;
    size_t s;
    bool ok;
  };

  TestCase tests[] = {
      {0, 0, true},
      {0, 1, true},
      {1, 1, true},
      {1 << 10, 0, true},
      {1 << 20, 0, true},
      {0, 1 << 10, true},
      {0, 1 << 20, true},
      {1 << 20, 2, true},
      {2, 1 << 20, true},
      {1000, 1000, true},
      {kMaxTestSize, 2, false},
      {2, kMaxTestSize, false},
      {kMaxTestSize, kMaxTestSize, false},
      {kMaxSignedSize, 3, false},
      {3, kMaxSignedSize, false},
      {kMaxSignedSize, kMaxSignedSize, false},
  };

  for (auto t : tests) {
    SCOPED_TRACE(absl::StrFormat("calloc(%x, %x)", t.n, t.s));

    void* ptr = calloc(t.n, t.s);
    benchmark::DoNotOptimize(ptr);

    EXPECT_EQ(t.ok, ptr != nullptr);
    if (ptr != nullptr) {
      memset(ptr, 0, t.n * t.s);
      benchmark::DoNotOptimize(ptr);
    }

    // This is harmless if p == nullptr.
    free(ptr);
  }
}

TEST(TcmallocTest, Realloc) {
  // Test that realloc doesn't always reallocate and copy memory.

  // When sampling, we always allocate in units of page-size, which makes
  // reallocs of small sizes do extra work (thus, failing these checks).  Since
  // sampling is random, we turn off sampling to make sure that doesn't happen
  // to us here.
  ScopedNeverSample never_sample;

  int start_sizes[] = {100, 1000, 10000, 100000};
  int deltas[] = {1, -2, 4, -8, 16, -32, 64, -128};

  for (int s = 0; s < sizeof(start_sizes) / sizeof(*start_sizes); ++s) {
    void* p = malloc(start_sizes[s]);
    // We stash a copy of the pointer p so we can reference it later.  We must
    // work with the return value of p.
    //
    // Even if we successfully determine that realloc's return value is
    // equivalent to its input value, we must use the returned value under
    // penalty of UB.
    const intptr_t orig_ptr = absl::bit_cast<intptr_t>(p);
    benchmark::DoNotOptimize(p);

    ASSERT_NE(p, nullptr);
    // The larger the start-size, the larger the non-reallocing delta.
    for (int d = 0; d < (s + 1) * 2; ++d) {
      p = realloc(p, start_sizes[s] + deltas[d]);
      const intptr_t new_ptr = absl::bit_cast<intptr_t>(p);
      benchmark::DoNotOptimize(p);

      ASSERT_EQ(orig_ptr, new_ptr)
          << ": realloc should not allocate new memory"
          << " (" << start_sizes[s] << " + " << deltas[d] << ")";
    }
    // Test again, but this time reallocing smaller first.
    for (int d = 0; d < s * 2; ++d) {
      p = realloc(p, start_sizes[s] - deltas[d]);
      const intptr_t new_ptr = absl::bit_cast<intptr_t>(p);
      benchmark::DoNotOptimize(p);

      ASSERT_EQ(orig_ptr, new_ptr)
          << ": realloc should not allocate new memory"
          << " (" << start_sizes[s] << " + " << -deltas[d] << ")";
    }
    free(p);
  }
}

TEST(TcmallocTest, MemalignRealloc) {
  constexpr size_t kDummySize = 42;
  char contents[kDummySize];
  memset(contents, 0x11, kDummySize);

  void* xs[2];
  xs[0] = memalign(16, kDummySize);
  ASSERT_EQ(0, posix_memalign(&xs[1], 16, kDummySize));

  for (void* x : xs) {
    memcpy(x, contents, sizeof(contents));

    ASSERT_NE(nullptr, x);
    void* y = realloc(x, 2 * kDummySize);
    // Reallocating memory obtained for memalign or posix_memalign should work.
    EXPECT_EQ(memcmp(contents, y, sizeof(contents)), 0);
    free(y);
  }
}

TEST(TCMallocTest, Multithreaded) {
  const int kThreads = 10;
  ThreadManager mgr;
  AllocatorHarness harness(kThreads);

  mgr.Start(kThreads, [&](int thread_id) { harness.Run(thread_id); });

  absl::SleepFor(absl::Seconds(5));

  mgr.Stop();
}

TEST(TCMallocTest, HugeThreadCache) {
  // Allocate more than 2^16 objects to trigger an integer overflow of 16-bit
  // counters.
  constexpr int kNum = 70000;
  constexpr size_t kSize = 10;
  std::vector<void*> arr;
  arr.reserve(kNum);

  for (int i = 0; i < kNum; i++) {
    arr.push_back(::operator new(kSize));
  }

  for (int i = 0; i < kNum; i++) {
    ::operator delete(arr[i], kSize);
  }
}

TEST(TCMallocTest, EnormousAllocations) {
  absl::BitGen rand;

  // Check that asking for stuff tiny bit smaller than largest possible
  // size returns NULL.
  for (size_t i = 0; i < 70000; i += absl::Uniform(rand, 1, 20)) {
    size_t size = kMaxTestSize - i;
    // Convince the compiler that size may change, as to suppress
    // optimization/warnings around the size being too large.
    benchmark::DoNotOptimize(size);
    void* p;

    p = malloc(size);
    ASSERT_EQ(nullptr, p);
    EXPECT_EQ(ENOMEM, errno);

    p = ::operator new(size, std::nothrow);
    ASSERT_EQ(nullptr, p);
    p = ::operator new(size, static_cast<std::align_val_t>(16), std::nothrow);
    ASSERT_EQ(nullptr, p);

    size_t alignment = sizeof(p) << absl::Uniform(rand, 1, kLogMaxMemalign);
    ASSERT_NE(0, alignment);
    ASSERT_EQ(0, alignment % sizeof(void*));
    ASSERT_TRUE(absl::has_single_bit(alignment)) << alignment;
    int err = PosixMemalign(&p, alignment, size);
    ASSERT_EQ(ENOMEM, err);
  }

  // Asking for memory sizes near signed/unsigned boundary (kMaxSignedSize)
  // might work or not, depending on the amount of virtual memory.
  for (size_t i = 0; i < 100; i++) {
    void* p;
    p = malloc(kMaxSignedSize + i);
    free(p);
    p = malloc(kMaxSignedSize - i);
    free(p);
  }

  for (size_t i = 0; i < 100; i++) {
    void* p;
    p = ::operator new(kMaxSignedSize + i, std::nothrow);
    ::operator delete(p);
    p = ::operator new(kMaxSignedSize - i, std::nothrow);
    ::operator delete(p);
  }

  // Check that ReleaseMemoryToSystem has no visible effect (aka, does not crash
  // the test):
  MallocExtension::ReleaseMemoryToSystem(std::numeric_limits<size_t>::max());
}

static size_t GetUnmappedBytes() {
  absl::optional<size_t> bytes =
      MallocExtension::GetNumericProperty("tcmalloc.pageheap_unmapped_bytes");
  CHECK_CONDITION(bytes.has_value());
  return *bytes;
}

TEST(TCMallocTest, ReleaseMemoryToSystem) {
  // Similarly, the hugepage-aware allocator doesn't agree with PH about
  // where release is called for.
  if (&tcmalloc::want_hpaa == nullptr || tcmalloc::want_hpaa()) {
    return;
  }

  static const int MB = 1048576;
  void* a = ::operator new(MB);
  void* b = ::operator new(MB);
  MallocExtension::ReleaseMemoryToSystem(std::numeric_limits<size_t>::max());
  size_t starting_bytes = GetUnmappedBytes();

  // Calling ReleaseMemoryToSystem() a second time shouldn't do anything.
  MallocExtension::ReleaseMemoryToSystem(std::numeric_limits<size_t>::max());
  EXPECT_EQ(starting_bytes, GetUnmappedBytes());

  // ReleaseMemoryToSystem shouldn't do anything either.
  MallocExtension::ReleaseMemoryToSystem(MB);
  EXPECT_EQ(starting_bytes, GetUnmappedBytes());

  ::operator delete(a);

  // The span to release should be 1MB.
  MallocExtension::ReleaseMemoryToSystem(MB / 2);
  EXPECT_EQ(starting_bytes + MB, GetUnmappedBytes());

  // Should do nothing since the previous call released too much.
  MallocExtension::ReleaseMemoryToSystem(MB / 4);
  EXPECT_EQ(starting_bytes + MB, GetUnmappedBytes());

  ::operator delete(b);

  // Use up the extra MB/4 bytes from 'a' and also release 'b'.
  MallocExtension::ReleaseMemoryToSystem(MB / 2);
  EXPECT_EQ(starting_bytes + 2 * MB, GetUnmappedBytes());

  // Should do nothing since the previous call released too much.
  MallocExtension::ReleaseMemoryToSystem(MB / 2);
  EXPECT_EQ(starting_bytes + 2 * MB, GetUnmappedBytes());

  // Nothing else to release.
  MallocExtension::ReleaseMemoryToSystem(std::numeric_limits<size_t>::max());
  EXPECT_EQ(starting_bytes + 2 * MB, GetUnmappedBytes());

  a = ::operator new(MB);
  ::operator delete(a);
  EXPECT_EQ(starting_bytes + MB, GetUnmappedBytes());

  // Releasing less than a page should still trigger a release.
  MallocExtension::ReleaseMemoryToSystem(1);
  EXPECT_EQ(starting_bytes + 2 * MB, GetUnmappedBytes());
}

TEST(TCMallocTest, MallocTrim) {
#ifdef TCMALLOC_HAVE_MALLOC_TRIM
  EXPECT_EQ(malloc_trim(0), 0);
#endif
}

TEST(TCMallocTest, NothrowSizedDelete) {
  struct Foo {
    double a;
  };
  // Foo should correspond to a size class used by new, but not by malloc.
  static_assert(sizeof(Foo) == 8, "Unexpected size for Foo");

  static constexpr int kNum = 100;
  Foo* ptrs[kNum];
  for (int i = 0; i < kNum; i++) {
    ptrs[i] = new (std::nothrow) Foo;
  }
  for (int i = 0; i < kNum; i++) {
    delete ptrs[i];
  }
}

TEST(TCMallocTest, NothrowSizedDeleteArray) {
  struct Foo {
    ~Foo() {}
    double a;
  };
  // Foo should correspond to a size class used by new, but not by malloc,
  // for some sizes k, sizeof(size_t) + sizeof(Foo) * k.  (sizeof(size_t) being
  // the size cookie of the implementation.)
  static_assert(sizeof(Foo) == 8, "Unexpected size for Foo");
  // With a non-trivially destructible type, we expect the compiler to insert a
  // size cookie so it can invoke sized delete[].
  static_assert(!std::is_trivially_destructible<Foo>::value,
                "Foo should not be trivially destructable, for sized delete[]");

  static constexpr int kNum = 100;
  Foo* ptrs[kNum];
  for (int i = 0; i < kNum; i++) {
    ptrs[i] = new (std::nothrow) Foo[i % 10];
  }
  for (int i = 0; i < kNum; i++) {
    delete[] ptrs[i];
  }
}

TEST(TCMallocTest, MallocAlignment) {
  static constexpr int kNum = 100;

  for (int lg = 0; lg < 16; lg++) {
    const size_t sizes[3] = {
        static_cast<size_t>((1 << lg) - 1),
        static_cast<size_t>(1 << lg),
        static_cast<size_t>((1 << lg) + 1),
    };
    void* ptrs[kNum * ABSL_ARRAYSIZE(sizes)];
    int i = 0;
    for (size_t size : sizes) {
      for (int j = 0; j < kNum; i++, j++) {
        ptrs[i] = malloc(size);
        uintptr_t p = reinterpret_cast<uintptr_t>(ptrs[i]);
        ASSERT_EQ(0, p % alignof(std::max_align_t)) << size << " " << j;
      }
    }

    for (void* ptr : ptrs) {
      free(ptr);
    }
  }
}

TEST(TCMallocTest, CallocAlignment) {
  static constexpr int kNum = 100;

  for (int lg = 0; lg < 16; lg++) {
    const size_t sizes[3] = {
        static_cast<size_t>((1 << lg) - 1),
        static_cast<size_t>(1 << lg),
        static_cast<size_t>((1 << lg) + 1),
    };
    void* ptrs[kNum * ABSL_ARRAYSIZE(sizes)];
    int i = 0;
    for (size_t size : sizes) {
      for (int j = 0; j < kNum; i++, j++) {
        ptrs[i] = calloc(size, (1 << (j % 5)));
        uintptr_t p = reinterpret_cast<uintptr_t>(ptrs[i]);
        ASSERT_EQ(0, p % alignof(std::max_align_t)) << size << " " << j;
      }
    }

    for (void* ptr : ptrs) {
      free(ptr);
    }
  }
}

TEST(TCMallocTest, ReallocAlignment) {
  static constexpr int kNum = 100;

  for (int lg = 0; lg < 16; lg++) {
    const size_t sizes[3] = {
        static_cast<size_t>((1 << lg) - 1),
        static_cast<size_t>(1 << lg),
        static_cast<size_t>((1 << lg) + 1),
    };
    void* ptrs[kNum * ABSL_ARRAYSIZE(sizes)];
    int i = 0;
    for (size_t size : sizes) {
      for (int j = 0; j < kNum; i++, j++) {
        ptrs[i] = malloc(size);
        uintptr_t p = reinterpret_cast<uintptr_t>(ptrs[i]);
        ASSERT_EQ(0, p % alignof(std::max_align_t)) << size << " " << j;

        const size_t new_size = (1 << (kNum % 16)) + (kNum % 3) - 1;
        void* new_ptr = realloc(ptrs[i], new_size);
        if (new_ptr == nullptr) {
          continue;
        }
        ptrs[i] = new_ptr;

        p = reinterpret_cast<uintptr_t>(new_ptr);
        ASSERT_EQ(0, p % alignof(std::max_align_t))
            << size << " -> " << new_size << " " << j;
      }
    }

    for (void* ptr : ptrs) {
      free(ptr);
    }
  }
}

TEST(TCMallocTest, AlignedNew) {
  absl::BitGen rand;

  struct alloc {
    void* ptr;
    size_t size;
    std::align_val_t alignment;
  };

  std::vector<alloc> allocated;
  for (int i = 1; i < 100; ++i) {
    alloc a;
    a.size = absl::LogUniform(rand, 0, 1 << 20);
    a.alignment = static_cast<std::align_val_t>(1 << absl::Uniform(rand, 0, 6));

    a.ptr = ::operator new(a.size, a.alignment);
    ASSERT_NE(a.ptr, nullptr);
    ASSERT_EQ(0, reinterpret_cast<uintptr_t>(a.ptr) %
                     static_cast<size_t>(a.alignment));
    allocated.emplace_back(a);
  }
  for (const auto& p : allocated) {
    int choice = absl::Uniform(rand, 0, 3);

    switch (choice) {
      case 0:
        ::operator delete(p.ptr);
        break;
      case 1:
        ::operator delete(p.ptr, p.alignment);
        break;
      case 2:
        ::operator delete(p.ptr, p.size, p.alignment);
        break;
    }
  }
}

TEST(TCMallocTest, AlignedNewArray) {
  absl::BitGen rand;

  struct alloc {
    void* ptr;
    size_t size;
    std::align_val_t alignment;
  };

  std::vector<alloc> allocated;
  for (int i = 1; i < 100; ++i) {
    alloc a;
    a.size = absl::LogUniform(rand, 0, 1 << 20);
    a.alignment = static_cast<std::align_val_t>(1 << absl::Uniform(rand, 0, 6));

    a.ptr = ::operator new[](a.size, a.alignment);
    ASSERT_NE(a.ptr, nullptr);
    ASSERT_EQ(0, reinterpret_cast<uintptr_t>(a.ptr) %
                     static_cast<size_t>(a.alignment));
    allocated.emplace_back(a);
  }
  for (const auto& p : allocated) {
    int choice = absl::Uniform(rand, 0, 3);

    switch (choice) {
      case 0:
        ::operator delete[](p.ptr);
        break;
      case 1:
        ::operator delete[](p.ptr, p.alignment);
        break;
      case 2:
        ::operator delete[](p.ptr, p.size, p.alignment);
        break;
    }
  }
}

TEST(TCMallocTest, NothrowAlignedNew) {
  absl::BitGen rand;
  for (int i = 1; i < 100; ++i) {
    size_t size = kMaxTestSize - i;
    std::align_val_t alignment =
        static_cast<std::align_val_t>(1 << absl::Uniform(rand, 0, 6));
    // Convince the compiler that size may change, as to suppress
    // optimization/warnings around the size being too large.
    benchmark::DoNotOptimize(size);
    void* p = ::operator new(size, alignment, std::nothrow);
    ASSERT_EQ(p, nullptr);
  }
  for (int i = 1; i < 100; ++i) {
    size_t size = absl::LogUniform(rand, 0, 1 << 20);
    std::align_val_t alignment =
        static_cast<std::align_val_t>(1 << absl::Uniform(rand, 0, 6));
    void* ptr = ::operator new(size, alignment, std::nothrow);
    ASSERT_NE(ptr, nullptr);
    ASSERT_EQ(
        0, reinterpret_cast<uintptr_t>(ptr) % static_cast<size_t>(alignment));
    ::operator delete(ptr, alignment, std::nothrow);
  }
}

TEST(TCMallocTest, NothrowAlignedNewArray) {
  absl::BitGen rand;
  for (int i = 1; i < 100; ++i) {
    size_t size = kMaxTestSize - i;
    std::align_val_t alignment =
        static_cast<std::align_val_t>(1 << absl::Uniform(rand, 0, 6));
    // Convince the compiler that size may change, as to suppress
    // optimization/warnings around the size being too large.
    benchmark::DoNotOptimize(size);
    void* p = ::operator new[](size, alignment, std::nothrow);
    ASSERT_EQ(p, nullptr);
  }
  for (int i = 1; i < 100; ++i) {
    size_t size = absl::LogUniform(rand, 0, 1 << 20);
    std::align_val_t alignment =
        static_cast<std::align_val_t>(1 << absl::Uniform(rand, 0, 6));
    void* ptr = ::operator new[](size, alignment, std::nothrow);
    ASSERT_NE(ptr, nullptr);
    ASSERT_EQ(
        0, reinterpret_cast<uintptr_t>(ptr) % static_cast<size_t>(alignment));
    ::operator delete[](ptr, alignment, std::nothrow);
  }
}

void CheckSizedDelete() {
  absl::BitGen rand;

  std::vector<std::pair<void*, size_t>> allocated;
  for (int i = 1; i < 100; ++i) {
    size_t alloc_size = absl::LogUniform<int32_t>(rand, 0, (1 << 20) - 1);
    void* p1 = ::operator new(alloc_size);
    ASSERT_NE(p1, nullptr);
    allocated.push_back(std::make_pair(p1, alloc_size));
  }
  for (std::vector<std::pair<void*, size_t>>::const_iterator i =
           allocated.begin();
       i != allocated.end(); ++i) {
    ::operator delete(i->first, i->second);
  }
}

TEST(TCMallocTest, SizedDelete) { CheckSizedDelete(); }

TEST(TCMallocTest, SizedDeleteSampled) {
  ScopedAlwaysSample always_sample;
  CheckSizedDelete();
}

// Check sampled allocations return the proper size.
TEST(TCMallocTest, SampleAllocatedSize) {
  ScopedAlwaysSample always_sample;

  // Do 64 megabytes of allocation; this should (nearly) guarantee we
  // get a sample.
  for (int i = 0; i < 1024 * 1024; ++i) {
    void* ptr = malloc(64);
    ASSERT_EQ(64, MallocExtension::GetAllocatedSize(ptr));
    free(ptr);
  }
}

// Ensure that nallocx works before main.
struct GlobalNallocx {
  GlobalNallocx() { CHECK_CONDITION(nallocx(99, 0) >= 99); }
} global_nallocx;

#ifdef __GNUC__
// 101 is the max user priority.
static void check_global_nallocx() __attribute__((constructor(101)));
static void check_global_nallocx() { CHECK_CONDITION(nallocx(99, 0) >= 99); }
#endif

TEST(TCMallocTest, nallocx) {
  // Guarded allocations may have a smaller allocated size than nallocx
  // predicts.  So we disable guarded allocations.
  ScopedGuardedSamplingRate gs(-1);

  for (size_t size = 0; size <= (1 << 20); size += 7) {
    size_t rounded = nallocx(size, 0);
    ASSERT_GE(rounded, size);
    void* ptr = operator new(size);
    ASSERT_EQ(rounded, MallocExtension::GetAllocatedSize(ptr));
    operator delete(ptr);
  }
}

TEST(TCMallocTest, nallocx_alignment) {
  // Guarded allocations may have a smaller allocated size than nallocx
  // predicts.  So we disable guarded allocations.
  ScopedGuardedSamplingRate gs(-1);

  for (size_t size = 0; size <= (1 << 20); size += 7) {
    for (size_t align = 0; align < 10; align++) {
      size_t rounded = nallocx(size, MALLOCX_LG_ALIGN(align));
      ASSERT_GE(rounded, size);
      ASSERT_EQ(rounded % (1 << align), 0);
      void* ptr = memalign(1 << align, size);
      ASSERT_EQ(rounded, MallocExtension::GetAllocatedSize(ptr));
      free(ptr);
    }
  }
}

TEST(TCMallocTest, sdallocx) {
  for (size_t size = 0; size <= 4096; size += 7) {
    void* ptr = malloc(size);
    memset(ptr, 0, size);
    benchmark::DoNotOptimize(ptr);
    sdallocx(ptr, size, 0);
  }
}

TEST(TCMallocTest, free_sized) {
  for (size_t size = 0; size <= 4096; size += 7) {
    void* ptr = malloc(size);
    memset(ptr, 0, size);
    benchmark::DoNotOptimize(ptr);
    free_sized(ptr, size);
  }
}

#ifndef NDEBUG
TEST(TCMallocTest, FreeSizedDeathTest) {
  void* ptr;
  const size_t size = 4096;
  const size_t alignment = 1024;
  int err = PosixMemalign(&ptr, 1024, alignment);
  ASSERT_EQ(err, 0) << alignment << " " << size;
  EXPECT_DEATH(free_sized(ptr, size), "");
}
#endif

TEST(TCMallocTest, free_aligned_aligned) {
  for (size_t size = 7; size <= 4096; size += 7) {
    for (size_t align = 0; align <= 10; align++) {
      const size_t alignment = 1 << align;
      void* ptr = aligned_alloc(alignment, size);
      ASSERT_NE(ptr, nullptr) << alignment << " " << size;
      ASSERT_EQ(reinterpret_cast<uintptr_t>(ptr) & (alignment - 1), 0);
      memset(ptr, 0, size);
      benchmark::DoNotOptimize(ptr);
      free_aligned_sized(ptr, alignment, size);
    }
  }
}

#ifndef NDEBUG
TEST(TCMallocTest, FreeAlignedSizedDeathTest) {
  const size_t size = 128;
  const size_t alignment = 1024;
  void* ptr = malloc(size);
  ASSERT_NE(ptr, nullptr) << alignment << " " << size;
  EXPECT_DEATH(free_aligned_sized(ptr, size, alignment), "");
}
#endif

TEST(TCMallocTest, sdallocx_alignment) {
  for (size_t size = 0; size <= 4096; size += 7) {
    for (size_t align = 3; align <= 10; align++) {
      const size_t alignment = 1 << align;
      void* ptr;
      int err = PosixMemalign(&ptr, alignment, size);
      ASSERT_EQ(err, 0) << alignment << " " << size;
      ASSERT_EQ(reinterpret_cast<uintptr_t>(ptr) & (alignment - 1), 0);
      memset(ptr, 0, size);
      benchmark::DoNotOptimize(ptr);
      sdallocx(ptr, size, MALLOCX_LG_ALIGN(align));
    }
  }
}

// Parse out a line like:
// <allocator_name>: xxx bytes allocated
// Return xxx as an int, nullopt if it can't be found
absl::optional<int64_t> ParseLowLevelAllocator(absl::string_view allocator_name,
                                               absl::string_view buf) {
  char needlebuf[32];
  int len =
      absl::SNPrintF(needlebuf, sizeof(needlebuf), "\n%s: ", allocator_name);
  CHECK_CONDITION(0 < len && len < sizeof(needlebuf));
  const absl::string_view needle = needlebuf;

  auto pos = buf.find(needle);
  if (pos == absl::string_view::npos) {
    return absl::nullopt;
  }
  // skip over the prefix.  Should now look like " <number> bytes allocated".
  pos += needle.size();
  buf.remove_prefix(pos);

  pos = buf.find_first_not_of(' ');
  if (pos != absl::string_view::npos) {
    buf.remove_prefix(pos);
  }

  pos = buf.find(' ');
  if (pos != absl::string_view::npos) {
    buf.remove_suffix(buf.size() - pos);
  }

  int64_t result;
  if (!absl::SimpleAtoi(buf, &result)) {
    return absl::nullopt;
  }
  return result;
}

TEST(TCMallocTest, GetStatsReportsLowLevel) {
  std::string stats = MallocExtension::GetStats();
  fprintf(stderr, "%s\n", stats.c_str());

  absl::optional<int64_t> low_level_bytes =
      ParseLowLevelAllocator("MmapSysAllocator", stats);
  ASSERT_THAT(low_level_bytes, testing::Ne(absl::nullopt));
  EXPECT_GT(*low_level_bytes, 0);
  size_t heap_size =
      *MallocExtension::GetNumericProperty("generic.current_allocated_bytes");

  // sanity check: we must have allocated as many bytes as in the heap
  EXPECT_GE(*low_level_bytes, heap_size);
}

#if defined(__GLIBC__) && defined(__GNUC__) && !defined(__MACH__)
namespace {
template <typename T1, typename T2>
void ExpectSameAddresses(T1 v1, T2 v2) {
  // C++ language requires a constant folding on constant inputs,
  // which may result to returning false for two aliased function,
  // because the aliasing is not known at this compilation unit.
  // Use volatile here to enforce a runtime comparison.
  volatile auto p1 = reinterpret_cast<void (*)()>(v1);
  volatile auto p2 = reinterpret_cast<void (*)()>(v2);
  const bool result = p1 == p2;
  // EXPECT_EQ seems not be able to handle volatiles.
  EXPECT_TRUE(result);
}
}  // end unnamed namespace

TEST(TCMallocTest, TestAliasedFunctions) {
  void* (*operator_new)(size_t) = &::operator new;
  void* (*operator_new_nothrow)(size_t, const std::nothrow_t&) =
      &::operator new;
  void* (*operator_new_array)(size_t) = &::operator new[];
  void* (*operator_new_array_nothrow)(size_t, const std::nothrow_t&) =
      &::operator new[];

  ExpectSameAddresses(operator_new, operator_new_array);
  ExpectSameAddresses(operator_new_nothrow, operator_new_array_nothrow);

  void (*operator_delete)(void*) = &::operator delete;
  void (*operator_delete_nothrow)(void*, const std::nothrow_t&) =
      &::operator delete;
  void (*operator_delete_array)(void*) = &::operator delete[];
  void (*operator_delete_array_nothrow)(void*, const std::nothrow_t&) =
      &::operator delete[];

  ExpectSameAddresses(&::free, operator_delete);
  ExpectSameAddresses(&::free, operator_delete_nothrow);
  ExpectSameAddresses(&::free, operator_delete_array);
  ExpectSameAddresses(&::free, operator_delete_array_nothrow);
}

#endif

enum class ThrowException { kNo, kYes };

class TcmallocSizedNewTest
    : public ::testing::TestWithParam<
          std::tuple<std::align_val_t, hot_cold_t, ThrowException>> {
 public:
  TcmallocSizedNewTest()
      : align_(std::get<0>(GetParam())),
        hot_cold_(std::get<1>(GetParam())),
        throw_exception_(std::get<2>(GetParam())) {
    const int align_needed = IsOveraligned();
    const int hot_cold_needed = (hot_cold_ != hot_cold_t{128});
    const int nothrow_needed = (throw_exception_ == ThrowException::kNo);
    const int encoding =
        (align_needed << 2) | (hot_cold_needed << 1) | nothrow_needed;
    switch (encoding) {
      case 0b000:
        sro_new_ = tcmalloc_size_returning_operator_new;
        break;
      case 0b001:
        sro_new_ = tcmalloc_size_returning_operator_new_nothrow;
        break;
      case 0b010:
        sro_new_ = [this](size_t size) {
          return tcmalloc_size_returning_operator_new_hot_cold(size, hot_cold_);
        };
        break;
      case 0b011:
        sro_new_ = [this](size_t size) {
          return tcmalloc_size_returning_operator_new_hot_cold_nothrow(
              size, hot_cold_);
        };
        break;
      case 0b100:
        sro_new_ = [this](size_t size) {
          return tcmalloc_size_returning_operator_new_aligned(size, align_);
        };
        break;
      case 0b101:
        sro_new_ = [this](size_t size) {
          return tcmalloc_size_returning_operator_new_aligned_nothrow(size,
                                                                      align_);
        };
        break;
      case 0b110:
        sro_new_ = [this](size_t size) {
          return tcmalloc_size_returning_operator_new_aligned_hot_cold(
              size, align_, hot_cold_);
        };
        break;
      case 0b111:
        sro_new_ = [this](size_t size) {
          return tcmalloc_size_returning_operator_new_aligned_hot_cold_nothrow(
              size, align_, hot_cold_);
        };
        break;
    }
  }

  sized_ptr_t New(size_t size) const { return sro_new_(size); }

  bool IsNothrow() const { return throw_exception_ == ThrowException::kNo; }

  bool IsOveraligned() const {
    return align_ > std::align_val_t{__STDCPP_DEFAULT_NEW_ALIGNMENT__};
  }

  std::align_val_t GetAlignment() const { return align_; }

  void Delete(sized_ptr_t res) const {
    if (IsOveraligned()) {
      ::operator delete(res.p, align_);
    } else {
      ::operator delete(res.p);
    }
  }

 private:
  std::align_val_t align_;
  hot_cold_t hot_cold_;
  ThrowException throw_exception_;
  // Size returning operator new.
  std::function<sized_ptr_t(size_t)> sro_new_;
};

INSTANTIATE_TEST_SUITE_P(
    AlignedHotColdThrow, TcmallocSizedNewTest,
    testing::Combine(
        testing::Values(1, 2, 4, 8, 16, 32, 64),
        testing::Values(hot_cold_t(0), hot_cold_t{128}, hot_cold_t{255}),
        testing::Values(ThrowException::kNo, ThrowException::kYes)),
    [](const testing::TestParamInfo<TcmallocSizedNewTest::ParamType>& info) {
      std::string name = absl::StrCat(
          "Align", std::get<0>(info.param), "HotCold",
          static_cast<int>(std::get<1>(info.param)),
          std::get<2>(info.param) == ThrowException::kNo ? "Nothrow" : "Throw");
      return name;
    });

TEST_P(TcmallocSizedNewTest, SizedOperatorNewReturnsExtraCapacity) {
  // For release / no sanitizer builds, tcmalloc does return
  // the next available class size, which we know is always at least
  // properly aligned, so size 3 should always return extra capacity.
  sized_ptr_t res = New(3);
  EXPECT_THAT(res.n, testing::Ge(8));
  Delete(res);
}

TEST_P(TcmallocSizedNewTest, SizedOperatorNew) {
  for (size_t size = 0; size < 64 * 1024; ++size) {
    sized_ptr_t res = New(size);
    EXPECT_NE(res.p, nullptr);
    EXPECT_GE(res.n, size);
    EXPECT_LE(size, std::max(size + 100, 2 * size));
    benchmark::DoNotOptimize(memset(res.p, 0xBF, res.n));
    Delete(res);
  }
}

TEST_P(TcmallocSizedNewTest, InvalidSizedOperatorNew) {
  constexpr size_t kBadSize = std::numeric_limits<size_t>::max();
  if (IsNothrow()) {
    sized_ptr_t res = New(kBadSize);
    EXPECT_EQ(res.p, nullptr);
    EXPECT_EQ(res.n, 0);
  } else {
    EXPECT_DEATH(New(kBadSize), "");
  }
}

TEST_P(TcmallocSizedNewTest, SizedOperatorNewMatchesMallocExtensionValue) {
  // Set reasonable sampling and guarded sampling probabilities.
  ScopedProfileSamplingRate s(20);
  ScopedGuardedSamplingRate gs(20);
  constexpr size_t kOddIncrement = 117;

  // Traverse clean power 2 / common size class / page sizes
  for (size_t size = 32; size <= 2 * 1024 * 1024; size *= 2) {
    sized_ptr_t r = New(size);
    ASSERT_EQ(r.n, MallocExtension::GetAllocatedSize(r.p));
    if (IsOveraligned()) {
      ::operator delete(r.p, r.n, GetAlignment());
    } else {
      ::operator delete(r.p, r.n);
    }
  }

  // Traverse randomized sizes
  for (size_t size = 32; size <= 2 * 1024 * 1024; size += kOddIncrement) {
    sized_ptr_t r = New(size);
    ASSERT_EQ(r.n, MallocExtension::GetAllocatedSize(r.p));
    if (IsOveraligned()) {
      ::operator delete(r.p, r.n, GetAlignment());
    } else {
      ::operator delete(r.p, r.n);
    }
  }
}

TEST(SizedDeleteTest, SizedOperatorDelete) {
  enum DeleteSize { kSize, kCapacity, kHalfway };
  for (size_t size = 0; size < 64 * 1024; ++size) {
    for (auto delete_size : {kSize, kCapacity, kHalfway}) {
      sized_ptr_t res = tcmalloc_size_returning_operator_new(size);
      switch (delete_size) {
        case kSize:
          ::operator delete(res.p, size);
          break;
        case kCapacity:
          ::operator delete(res.p, res.n);
          break;
        case kHalfway:
          ::operator delete(res.p, (size + res.n) / 2);
          break;
      }
    }
  }
}

TEST(SizedDeleteTest, NothrowSizedOperatorDelete) {
  for (size_t size = 0; size < 64 * 1024; ++size) {
    sized_ptr_t res = tcmalloc_size_returning_operator_new(size);
    ::operator delete(res.p, std::nothrow);
  }
}

TEST(HotColdTest, HotColdNew) {
  const bool expectColdTags = tcmalloc_internal::ColdFeatureActive();
  using tcmalloc_internal::IsColdMemory;
  using tcmalloc_internal::IsSampledMemory;

  absl::flat_hash_set<uintptr_t> hot;
  absl::flat_hash_set<uintptr_t> cold;

  absl::BitGen rng;

  // Allocate some hot objects
  struct SizedPtr {
    void* ptr;
    size_t size;
  };

  constexpr size_t kSmall = 2 << 10;
  constexpr size_t kLarge = 1 << 20;

  std::vector<SizedPtr> ptrs;
  ptrs.reserve(1000);
  for (int i = 0; i < 1000; i++) {
    size_t size = absl::LogUniform<size_t>(rng, kSmall, kLarge);
    uint8_t label = absl::Uniform<uint8_t>(rng, 0, 127);
    label |= uint8_t{128};

    void* ptr = ::operator new(size, static_cast<tcmalloc::hot_cold_t>(label));

    ptrs.emplace_back(SizedPtr{ptr, size});

    EXPECT_TRUE(!IsColdMemory(ptr)) << ptr;
  }

  // Delete
  for (SizedPtr s : ptrs) {
    if (expectColdTags && !IsSampledMemory(s.ptr)) {
      EXPECT_TRUE(hot.insert(reinterpret_cast<uintptr_t>(s.ptr)).second);
    }

    if (absl::Bernoulli(rng, 0.2)) {
      ::operator delete(s.ptr);
    } else {
      sized_delete(s.ptr, s.size);
    }
  }

  // Allocate some cold objects
  ptrs.clear();
  for (int i = 0; i < 1000; i++) {
    size_t size = absl::LogUniform<size_t>(rng, kSmall, kLarge);
    uint8_t label = absl::Uniform<uint8_t>(rng, 0, 127);
    label &= ~uint8_t{128};

    void* ptr = ::operator new(size, static_cast<tcmalloc::hot_cold_t>(label));
    ptrs.emplace_back(SizedPtr{ptr, size});
  }

  for (SizedPtr s : ptrs) {
    if (expectColdTags && IsColdMemory(s.ptr)) {
      EXPECT_TRUE(cold.insert(reinterpret_cast<uintptr_t>(s.ptr)).second);
    }

    if (absl::Bernoulli(rng, 0.2)) {
      ::operator delete(s.ptr);
    } else {
      sized_delete(s.ptr, s.size);
    }
  }

  if (!expectColdTags) {
    return;
  }

  for (uintptr_t h : hot) {
    EXPECT_THAT(cold, testing::Not(testing::Contains(h)))
        << reinterpret_cast<void*>(h);
  }
}

TEST(HotColdTest, SizeReturningHotColdNew) {
  const bool expectColdTags = tcmalloc_internal::ColdFeatureActive();
  if (!expectColdTags) {
    GTEST_SKIP() << "Cold allocations not enabled";
  }
  using tcmalloc_internal::IsColdMemory;
  using tcmalloc_internal::IsSampledMemory;

  constexpr size_t kSmall = 128 << 10;
  constexpr size_t kLarge = 1 << 20;

  absl::BitGen rng;

  // Allocate some objects
  struct SizedPtr {
    void* ptr;
    size_t requested;
    size_t actual;
  };

  std::vector<SizedPtr> ptrs;
  ptrs.reserve(1000);
  for (int i = 0; i < 1000; i++) {
    const size_t requested = absl::LogUniform<size_t>(rng, kSmall, kLarge);
    const uint8_t label = absl::Uniform<uint8_t>(rng, 0, 255);

    auto [ptr, actual] = tcmalloc_size_returning_operator_new_hot_cold(
        requested, static_cast<hot_cold_t>(label));
    ASSERT_GE(actual, requested);

    if (label >= 128) {
      // Hot
      EXPECT_FALSE(IsColdMemory(ptr));
    } else {
      EXPECT_TRUE(IsSampledMemory(ptr) || IsColdMemory(ptr))
          << requested << " " << label;
    }

    std::optional<size_t> allocated_size =
        MallocExtension::GetAllocatedSize(ptr);
    ASSERT_THAT(allocated_size, testing::Ne(std::nullopt));
    EXPECT_EQ(actual, *allocated_size);

    ptrs.emplace_back(SizedPtr{ptr, requested, actual});
  }

  for (auto s : ptrs) {
    const double coin = absl::Uniform(rng, 0., 1.);

    if (coin < 0.2) {
      ::operator delete(s.ptr);
    } else if (coin < 0.4) {
      // Exact size.
      sized_delete(s.ptr, s.actual);
    } else if (coin < 0.6) {
      sized_delete(s.ptr, s.requested);
    } else {
      sized_delete(s.ptr, absl::Uniform(rng, s.requested, s.actual));
    }
  }
}

// Test that when we use size-returning new, we can pass any of the sizes
// between the requested size and the allocated size to sized-delete.
// We follow
// https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2022/p0901r9.html#sizeddelete.
TEST(MallocExtension, SizeReturningNewAndSizedDelete) {
#ifndef __cpp_sized_deallocation
  GTEST_SKIP() << "No sized deallocation.";
#else
  for (int i = 0; i < 100; ++i) {
    tcmalloc::sized_ptr_t sized_ptr = tcmalloc_size_returning_operator_new(i);
    ::operator delete(sized_ptr.p, sized_ptr.n);
    for (int j = i, end = sized_ptr.n; j < end; ++j) {
      sized_ptr = tcmalloc_size_returning_operator_new(i);
      EXPECT_EQ(end, sized_ptr.n) << i << "," << j;
      ::operator delete(sized_ptr.p, j);
    }
  }
#endif
}

TEST(TCMalloc, malloc_info) {
  char* buf = nullptr;
  size_t size = 0;
  FILE* fp = open_memstream(&buf, &size);
  ASSERT_NE(fp, nullptr);
  EXPECT_EQ(malloc_info(0, fp), 0);
  EXPECT_EQ(fclose(fp), 0);
  ASSERT_NE(buf, nullptr);
  EXPECT_EQ(absl::string_view(buf, size), "<malloc></malloc>\n");
  free(buf);
}

}  // namespace
}  // namespace tcmalloc
