// Wrapper: compile the shared config_store.cpp with the production build's
// full include paths (so <Preferences.h> and other framework headers resolve).
// The unity build no longer includes this file to avoid duplicate symbols.
// The guard matches the other Bahnbrett sources: the stock environments
// carry no -I../../include, so the include below cannot resolve there.
#if defined(BAHNBRETT_LOCAL_RENDER)
#include "../../../src/config_store.cpp"
#endif
