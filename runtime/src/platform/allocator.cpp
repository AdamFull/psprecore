// Global operator new/delete go to mimalloc (brought in by the nx foundation library): the renderers, the HLE
// layer and the containers allocate small blocks at a high rate. C allocations (malloc, SDL, ImGui) are unchanged.

#include <mimalloc-new-delete.h>
