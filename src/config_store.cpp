// Wrapper: compile the shared config_store.cpp with the production build's
// full include paths (so <Preferences.h> and other framework headers resolve).
// The unity build no longer includes this file to avoid duplicate symbols.
#include "../../../src/config_store.cpp"
