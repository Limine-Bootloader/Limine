#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static void preprocess(char *config_addr, size_t *config_size) {
    size_t write = 0;
    for (size_t read = 0; read < *config_size; read++) {
        if (config_addr[read] == '\\' && read + 1 < *config_size) {
            if (config_addr[read + 1] == '\n') { read++; continue; }
            if (config_addr[read + 1] == '\r' && read + 2 < *config_size && config_addr[read + 2] == '\n') { read += 2; continue; }
        }
        config_addr[write++] = config_addr[read];
    }
    *config_size = write;
}

static int failures = 0;

static void test_str(const char *name, const char *input, const char *expected) {
    size_t sz = strlen(input);
    char *buf = malloc(sz + 1);

    memcpy(buf, input, sz); buf[sz] = '\0';
    preprocess(buf, &sz); buf[sz] = '\0';

    if (strcmp(buf, expected) != 0) { printf("FAIL: %s\n", name); failures++; }
    else printf("PASS: %s\n", name);
    free(buf);
}

int main(void) {
    test_str("unix continuation", "A\\\nB\n", "AB\n");
    test_str("windows continuation", "A\\\r\nB\n", "AB\n");
    test_str("no continuation", "AB\n", "AB\n");

    test_str("trailing backslash", "A\\", "A\\");
    test_str("double backslash+cont", "A\\\\\\\nB\n", "A\\\\B\n");
    test_str("multiple continuations", "a\\\nb\\\nc\n", "abc\n");

    test_str("empty continuation line", "a\\\n\\\nb\n", "ab\n");
    test_str("normal text", "hello\n", "hello\n");
    test_str("backslash before char", "a\\b\n", "a\\b\n");
    test_str("continuation at start", "\\\nhello\n", "hello\n");

    test_str("continuation at end", "hello\\", "hello\\");
    test_str("spaces after backslash", "a\\   \nb\n", "a\\   \nb\n");
    test_str("double continuation", "a\\\n\\\nb\n", "ab\n");
    test_str("triple continuation", "a\\\n\\\n\\\nb\n", "ab\n");
    
    test_str("empty string", "", "");
    test_str("only newline", "\n", "\n");
    test_str("only CRLF", "\r\n", "\r\n");

    { char *b=malloc(3000); size_t p=0;
      for(int i=0;i<1000;i++){b[p++]='\\';b[p++]='\n';}
      b[p++]='X';b[p]=0;size_t s=p;preprocess(b,&s);b[s]=0;
      if(strcmp(b,"X")==0)printf("PASS: 1000 continuations\n");else{failures++;printf("FAIL: 1000 continuations\n");}free(b); }

    { char *b=malloc(200001);size_t p=0;
      for(int i=0;i<100000;i++){b[p++]='\\';b[p++]='\n';}
      b[p++]='Z';b[p]=0;size_t s=p;preprocess(b,&s);b[s]=0;
      if(strcmp(b,"Z")==0)printf("PASS: 100K continuations\n");else{failures++;printf("FAIL: 100K continuations\n");}free(b); }

    { srand(42);int ok=1;
      for(int t=0;t<100000&&ok;t++){char b[2048];
        for(int i=0;i<2047;i++){switch(rand()%8){case 0:b[i]='\\';break;case 1:b[i]='\n';break;case 2:b[i]='\r';break;default:b[i]='A'+rand()%26;}}
        b[2047]=0;size_t s=2047;preprocess(b,&s);if(s>2047)ok=0;}
      if(ok)printf("PASS: fuzz test (100K random)\n");else{failures++;printf("FAIL: fuzz test\n");} }

    printf("\n%d failures\n", failures);
    return failures;
}