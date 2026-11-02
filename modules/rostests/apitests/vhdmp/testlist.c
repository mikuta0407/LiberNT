#define STANDALONE
#include <apitest.h>

extern void func_VhdBoot(void);

const struct test winetest_testlist[] =
{
    { "VhdBoot", func_VhdBoot },
    { 0, 0 }
};
