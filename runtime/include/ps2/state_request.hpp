#pragma once

namespace ps2 {

// Queue a state operation from the UI thread. It runs after generated code has
// unwound, on the emulation thread. Only one request may be outstanding.
// The completion callback also runs on the emulation thread.
bool request_state_operation(bool loading, const char *path, void (*complete)(bool loading, bool success));

}
