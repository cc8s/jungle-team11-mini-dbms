#include "test.h"
#include "test_util.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

/* 테스트 도우미(test_util.h) 자체를 검증한다. 이후 모든 테스트가 이 도우미에 기대므로 먼저 지켜 둔다. */

static bool path_exists(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0;
}

static bool write_empty_file(const char *dir, const char *name, char *out_path, size_t path_size)
{
    snprintf(out_path, path_size, "%s/%s", dir, name);
    int fd = open(out_path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0)
        return false;
    close(fd);
    return true;
}

/* T00-01 */ static void test_temp_dir_exists_until_removed(void)
{
    char *dir = test_make_temp_dir();
    CHECK(dir != NULL);
    CHECK(path_exists(dir));

    CHECK(test_remove_dir(dir));
    CHECK(!path_exists(dir));
    free(dir);
}

/* T00-02 */ static void test_remove_dir_deletes_nested_files(void)
{
    char *dir = test_make_temp_dir();
    CHECK(dir != NULL);

    char sub_dir[TEST_PATH_MAX];
    char top_file[TEST_PATH_MAX];
    char nested_file[TEST_PATH_MAX];
    snprintf(sub_dir, sizeof(sub_dir), "%s/sub", dir);
    CHECK(mkdir(sub_dir, 0755) == 0);
    CHECK(write_empty_file(dir, "users.tbl", top_file, sizeof(top_file)));
    CHECK(write_empty_file(sub_dir, "nested.tbl", nested_file, sizeof(nested_file)));

    CHECK(test_remove_dir(dir));
    CHECK(!path_exists(nested_file));
    CHECK(!path_exists(top_file));
    CHECK(!path_exists(dir));
    free(dir);
}

int main(void)
{
    RUN(test_temp_dir_exists_until_removed);
    RUN(test_remove_dir_deletes_nested_files);
    TEST_DONE();
    return 0;
}
