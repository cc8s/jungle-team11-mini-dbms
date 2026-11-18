/* users 테이블용 INSERT 문 생성기 (#8).
 *
 *   ./tools/gen_data 1000000 [seed] > /tmp/users.sql
 *   ./minidb -d data /tmp/users.sql > /dev/null
 *
 * i 번째 행: name = 'user<i>', email = 'user<i>@jungle.kr', age = 18..77 (고정 시드 의사 난수).
 * 같은 N, 같은 seed 면 같은 파일이 나온다. INSERT 는 id 를 정하지 않으므로 빈 테이블에 넣으면 id = i 다. */

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <rows> [seed]\n", argv[0]);
        return 2;
    }
    long rows = strtol(argv[1], NULL, 10);
    unsigned long seed = argc > 2 ? strtoul(argv[2], NULL, 10) : 42;

    if (rows < 1) {
        fprintf(stderr, "rows must be positive\n");
        return 2;
    }
    for (long i = 1; i <= rows; i++) {
        seed = seed * 6364136223846793005UL + 1442695040888963407UL; /* 64비트 LCG */
        int age = 18 + (int)((seed >> 33) % 60);
        printf("INSERT INTO users VALUES ('user%ld', 'user%ld@jungle.kr', %d);\n", i, i, age);
    }
    return 0;
}
