#include <stdio.h>

static int twice(int n)
{
    int r = n * 2;
    return r;
}

int add(int a, int b)
{
    int c = a + b;
    c = twice(c);
    return c;
}

int main()
{
    int x = add(1, 2);
    printf("x=%d\n", x);
    int y = add(x, 4);
    printf("y=%d\n", y);
    return y - 14;
}
