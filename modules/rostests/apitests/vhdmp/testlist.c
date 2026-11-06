#define STANDALONE
#include <apitest.h>

extern void func_VhdBcd(void);
extern void func_VhdBoot(void);

const struct test winetest_testlist[] =
{
    { "VhdBcd", func_VhdBcd },
    { "VhdBoot", func_VhdBoot },
    { 0, 0 }
};
