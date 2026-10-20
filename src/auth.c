#include "ops.h"

#include <crypt.h>
#include <string.h>

int hash_password(const char *password, char *out, size_t size)
{
    struct crypt_data data;
    char salt[CRYPT_GENSALT_OUTPUT_SIZE];

    /* SHA-512 crypt with a random salt from the kernel's entropy source. */
    if (!crypt_gensalt_rn("$6$", 0, NULL, 0, salt, sizeof(salt)))
        return -1;
    memset(&data, 0, sizeof(data));
    const char *h = crypt_r(password, salt, &data);
    if (!h || h[0] == '*' || strlen(h) >= size)
        return -1;
    strcpy(out, h);
    return 0;
}

int verify_password(const char *password, const char *hash)
{
    struct crypt_data data;

    if (hash[0] == '\0')
        return 0;
    memset(&data, 0, sizeof(data));
    const char *h = crypt_r(password, hash, &data);
    return h && h[0] != '*' && strcmp(h, hash) == 0;
}
