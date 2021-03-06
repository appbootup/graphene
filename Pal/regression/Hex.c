#include <assert.h>
#include "pal_debug.h"

#include "hex.h"

#include "api.h"
#include "pal.h"

noreturn void __abort(void) {
    // ENOTRECOVERABLE = 131
    DkProcessExit(-131);
}

int main(void) {
    char x[] = {0xde, 0xad, 0xbe, 0xef};
    char y[] = {0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd, 0xcd};
    pal_printf("Hex test 1 is %s\n", ALLOCA_BYTES2HEXSTR(x));
    pal_printf("Hex test 2 is %s\n", ALLOCA_BYTES2HEXSTR(y));
    return 0;
}
