// module/opsec/call_gate.hpp — §6.2.
// A gate is a tiny stub (mov rax, <fn>; jmp rax) written into an RX code
// cave of a legit loaded game module (client.dll padding, after a function
// epilogue). Our outbound call lands in that stub first — return address
// sits inside the legit module, so the ret-tracker (msg 164) sees nothing
// odd.
#pragma once
#include <cstddef>

namespace cheat::opsec {

// Returns the gate address (equal to a function pointer of the same signature
// as `realFn`). nullptr on failure (no caves in the chosen module).
void* MakeGateToClient(void* realFn);

// Cave pool stats, for the Settings debug tab.
struct CaveStats { std::size_t used; std::size_t capacity; };
CaveStats PoolStats();

void UnloadAllGates();

} // namespace cheat::opsec
