#include "host.h"
int main(void) {
    return hostfn_override("absent") || hostfn_observer("absent") || hostfn_table_size(0) || hostfn_table_size(1);
}
