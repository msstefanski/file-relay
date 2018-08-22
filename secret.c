#include <unistd.h>
#include <openssl/sha.h>
#include <sys/syscall.h>
#include <linux/random.h>
#include "secret.h"

char *make_secret(int num_words)
{
    char c;
    char *line;
    char *words = malloc((num_words + 1)*45);
    ssize_t line_read;
    ssize_t line_alloced;

    FILE *f = fopen("/usr/share/dict/words", "r");
    if (!f) {
        fprintf(stderr, "Failed to open /usr/share/dict/words\n");
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) < 0) {
        fprintf(stderr, "Failed to seek to end of file\n");
        return NULL;
    }

    long size = ftell(f);

    unsigned int seed = 0;
    syscall(SYS_getrandom, &seed, sizeof(unsigned int), 0);
    srandom(seed);

    for (int w = 0; w < num_words; ++w) {
        //get random and scale to dictionary file size
        long r = random();
        double scale = (double)r / RAND_MAX;
        long seek_pos = (long) (size * scale);

        //seek to random place in the dictionary, read until newline, then get the next line
        fseek(f, seek_pos, SEEK_SET);
        while (fread(&c, 1, 1, f) == 1 && c != '\n');
        if (c != '\n') {
            fprintf(stderr, "Failed to read from dictionary\n");
            break;
        }

        line = NULL;
        line_alloced = 0;
        line_read = getline(&line, &line_alloced, f);
        if (line_read) {
            if (w < num_words - 1)
                line[line_read-1] = '-';
            else
                line[line_read-1] = '\0';
            strncat(words, line, line_read);
        }
        free(line);
    }

    fclose(f);

    char *quote = strstr(words, "'");
    while (quote != NULL) {
        quote[0] = 'Q';
        quote = strstr(words, "'");
    }

    return words;
}

char *make_hash(const char *secret)
{
    SHA_CTX ctx;
    SHA1_Init(&ctx);
    SHA1_Update(&ctx, secret, strlen(secret));
    unsigned char rawhash[SHA_DIGEST_LENGTH];
    SHA1_Final(rawhash, &ctx);

    unsigned char *readable_hash = malloc(SHA_DIGEST_LENGTH*2 + 1);
    for (int i=0; i < SHA_DIGEST_LENGTH; ++i) {
        sprintf((char*)&(readable_hash[i*2]), "%02x", rawhash[i]);
    }
    readable_hash[SHA_DIGEST_LENGTH*2 - 1] = '\0';

    return readable_hash;
}

