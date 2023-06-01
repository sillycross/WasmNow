#include <cstdio>
#include <cstdlib> 

#include "codegen_context.hpp"

void test_main(char* filename);

int main(int argc, char **argv)
{
  if (argc <= 1) {
    printf("wasmnow: no input files\n");
    return 0;
  }
  
  test_main(argv[1]);
  return 0;
}

