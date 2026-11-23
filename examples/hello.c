/* Slyztrix 首个 musl 验证程序：stdio + malloc + string + 退出码。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    printf("Hello from musl on SaylterOS\n");
    printf("argc=%d argv0=%s\n", argc, argv[0]);

    char *buf = (char *)malloc(256);
    if (!buf) {
        printf("malloc failed\n");
        return 2;
    }
    strcpy(buf, "malloc works");
    printf("%s, len=%d\n", buf, (int)strlen(buf));
    free(buf);

    printf("exit code 0\n");
    return 0;
}
