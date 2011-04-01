// Copyright 2011 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "v8.h"

#include "isolate.h"
#include "bootstrapper.h"
#include "debug.h"
#include "deoptimizer.h"
#include "heap-profiler.h"
#include "hydrogen.h"
#include "lithium-allocator.h"
#include "log.h"
#include "runtime-profiler.h"
#include "serialize.h"

namespace v8 {
namespace internal {

static Mutex* init_once_mutex = OS::CreateMutex();
static bool init_once_called = false;

bool V8::is_running_ = false;
bool V8::has_been_setup_ = false;
bool V8::has_been_disposed_ = false;
bool V8::has_fatal_error_ = false;
bool V8::use_crankshaft_ = true;


bool V8::Initialize(Deserializer* des) {
  InitializeOncePerProcess();

  // The current thread may not yet had entered an isolate to run.
  // Note the Isolate::Current() may be non-null because for various
  // initialization purposes an initializing thread may be assigned an isolate
  // but not actually enter it.
  if (i::Isolate::CurrentPerIsolateThreadData() == NULL) {
    i::Isolate::EnterDefaultIsolate();
  }

  ASSERT(i::Isolate::CurrentPerIsolateThreadData() != NULL);
  ASSERT(i::Isolate::CurrentPerIsolateThreadData()->thread_id() ==
         i::Thread::GetThreadLocalInt(i::Isolate::thread_id_key()));
  ASSERT(i::Isolate::CurrentPerIsolateThreadData()->isolate() ==
         i::Isolate::Current());

  if (IsDead()) return false;

  Isolate* isolate = Isolate::Current();
  if (isolate->IsInitialized()) return true;

  is_running_ = true;
  has_been_setup_ = true;
  has_fatal_error_ = false;
  has_been_disposed_ = false;

  return isolate->Init(des);
}


void V8::SetFatalError() {
  is_running_ = false;
  has_fatal_error_ = true;
}


void V8::TearDown() {
  Isolate* isolate = Isolate::Current();
  ASSERT(isolate->IsDefaultIsolate());

  if (!has_been_setup_ || has_been_disposed_) return;
  isolate->TearDown();

  is_running_ = false;
  has_been_disposed_ = true;
}


static uint32_t random_seed() {
  if (FLAG_random_seed == 0) {
    return random();
  }
  return FLAG_random_seed;
}


typedef struct {
  uint32_t hi;
  uint32_t lo;
} random_state;


// Random number generator using George Marsaglia's MWC algorithm.
static uint32_t random_base(random_state *state) {
  // Initialize seed using the system random(). If one of the seeds
  // should ever become zero again, or if random() returns zero, we
  // avoid getting stuck with zero bits in hi or lo by re-initializing
  // them on demand.
  if (state->hi == 0) state->hi = random_seed();
  if (state->lo == 0) state->lo = random_seed();

  // Mix the bits.
  state->hi = 36969 * (state->hi & 0xFFFF) + (state->hi >> 16);
  state->lo = 18273 * (state->lo & 0xFFFF) + (state->lo >> 16);
  return (state->hi << 16) + (state->lo & 0xFFFF);
}


// Used by JavaScript APIs
uint32_t V8::Random(Isolate* isolate) {
  ASSERT(isolate == Isolate::Current());
  // TODO(isolates): move lo and hi to isolate
  static random_state state = {0, 0};
  return random_base(&state);
}


// Used internally by the JIT and memory allocator for security
// purposes. So, we keep a different state to prevent informations
// leaks that could be used in an exploit.
uint32_t V8::RandomPrivate(Isolate* isolate) {
  ASSERT(isolate == Isolate::Current());
  // TODO(isolates): move lo and hi to isolate
  static random_state state = {0, 0};
  return random_base(&state);
}


bool V8::IdleNotification() {
  // Returning true tells the caller that there is no need to call
  // IdleNotification again.
  if (!FLAG_use_idle_notification) return true;

  // Tell the heap that it may want to adjust.
  return HEAP->IdleNotification();
}


// Use a union type to avoid type-aliasing optimizations in GCC.
typedef union {
  double double_value;
  uint64_t uint64_t_value;
} double_int_union;


Object* V8::FillHeapNumberWithRandom(Object* heap_number, Isolate* isolate) {
  uint64_t random_bits = Random(isolate);
  // Make a double* from address (heap_number + sizeof(double)).
  double_int_union* r = reinterpret_cast<double_int_union*>(
      reinterpret_cast<char*>(heap_number) +
      HeapNumber::kValueOffset - kHeapObjectTag);
  // Convert 32 random bits to 0.(32 random bits) in a double
  // by computing:
  // ( 1.(20 0s)(32 random bits) x 2^20 ) - (1.0 x 2^20)).
  const double binary_million = 1048576.0;
  r->double_value = binary_million;
  r->uint64_t_value |=  random_bits;
  r->double_value -= binary_million;

  return heap_number;
}


void V8::InitializeOncePerProcess() {
  ScopedLock lock(init_once_mutex);
  if (init_once_called) return;
  init_once_called = true;

  // Setup the platform OS support.
  OS::Setup();

#if defined(V8_TARGET_ARCH_ARM) && !defined(USE_ARM_EABI)
  use_crankshaft_ = false;
#else
  use_crankshaft_ = true;
#endif

  if (Serializer::enabled()) {
    use_crankshaft_ = false;
  }

  CPU::Setup();
  if (!CPU::SupportsCrankshaft()) {
    use_crankshaft_ = false;
  }

  // Peephole optimization might interfere with deoptimization.
  FLAG_peephole_optimization = !use_crankshaft_;
}

} }  // namespace v8::internal
