#pragma once

namespace cheat::opsec {

// Overwrite MZ + e_lfanew in the mapped image. Idempotent.
// (Injector also does this if options.eraseHeaders; the module running it
// post-boot catches late maps and failed-injector cleanup.)
void EraseHeaders();

// True iff every required offset in common/offsets.hpp is non-zero.
bool OffsetsVerified();

// For the module-side settings tab.
void SetDegradedMask(unsigned mask);
unsigned DegradedMask();

} // namespace cheat::opsec
