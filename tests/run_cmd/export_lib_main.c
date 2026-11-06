// A C program over the Eskiu library export_lib.esk, through its generated header.
#include <stdio.h>
#include "export_lib.h"

int main(void) {
    Body b = body_make(7, Run);
    Node n2 = {5, NULL};
    Node n1 = {4, &n2};
    Wire w = {2, 40};
    Cell c = cell_of(0.25);
    printf("%u %d %d %d %d %d\n", b.id, b.mode, b.flags.ready, b.flags.level, b.flags.on, b.slots[5]);
    printf("%.1f %d %.2f %lld %d %d\n", body_len(&b), node_sum(&n1), c.d, (long long)wire_stamp(w),
           next_mode(Idle), next_mode(Run) == Stop);
    size_t c_sizes[5] = {sizeof(Body), sizeof(Flags), sizeof(Wire), sizeof(Cell), sizeof(Node)};
    for (int i = 0; i < 5; i++)
        printf("%s%s", (int64_t)c_sizes[i] == size_of(i) ? "same" : "DIFFERENT", i < 4 ? " " : "\n");
    return 0;
}
