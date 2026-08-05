#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static void preprocess_new(char *config_addr, size_t *config_size) {
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

static void preprocess_old(char *config_addr, size_t *config_size) {
   
    for (size_t i = 0; i < *config_size - 1; i++) {
        if (config_addr[i] == '\\' && config_addr[i + 1] == '\n') {
            for (size_t j = i; j < *config_size - 2; j++) config_addr[j] = config_addr[j + 2];
            *config_size -= 2; i--;
        }
         else if (config_addr[i] == '\\' && config_addr[i + 1] == '\r' && i + 2 < *config_size && config_addr[i + 2] == '\n') {
            for (size_t j = i; j < *config_size - 3; j++) config_addr[j] = config_addr[j + 3];
            *config_size -= 3; i--;
        }
    }
}

int main(void) {
    srand(42);
    int mismatches = 0;
    for (int t = 0; t < 50000; t++) {
        size_t len = 32 + rand() % 256;
        char *input = malloc(len + 1);
      
        char *buf_old = malloc(len + 1);
        char *buf_new = malloc(len + 1);
       
        for (size_t i = 0; i < len; i++) {
           
            switch (rand() % 10) {
                case 0: input[i] = '\\'; break;
                case 1: input[i] = '\n'; break;
                case 2: input[i] = '\r'; break;
                default: input[i] = 'A' + rand() % 26;
            }
        }
     
        input[len] = '\0';
        memcpy(buf_old, input, len); memcpy(buf_new, input, len);
     
        size_t sz_old = len, sz_new = len;
        preprocess_old(buf_old, &sz_old);
      
        preprocess_new(buf_new, &sz_new);
        if (sz_old != sz_new || memcmp(buf_old, buf_new, sz_old) != 0) mismatches++;
        free(input); free(buf_old); free(buf_new);
    }
  
    if (mismatches == 0) printf("PASS: 50000 random comparisons match\n");
    else printf("FAIL: %d mismatches\n", mismatches);
    return mismatches;
}