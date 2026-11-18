// This archive member has no referenced external symbol. Its declarations must survive extraction and dead stripping.
extern "C" {
#include "host.h"
}
static void deeper(Cpu *c) { c->x[0] *= 2; }
HOST_OVERRIDE("archive", deeper);
HOST_OVERRIDE("duplicate", deeper);
HOST_OBSERVER("observed", deeper);
HOST_FUNCTION("twice", deeper);
