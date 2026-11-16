/*
 * equip_main.c - 설비 프로세스 실행 파일 (equip).
 * 시설 프로세스가 execv 로 띄운다:  argv = { <설비 이름>, <시설 폴더 이름>, <시설 pid> }
 */
#include <stdio.h>
#include <stdlib.h>

#include "equipment.h"

int main(int argc, char **argv)
{
    int fac;

    if (argc < 3 || (fac = facility_from_key(argv[1])) < 0) {
        fprintf(stderr, "usage: equip <generator|cooling|power> <parent pid>\n");
        return 2;
    }
    return equip_main(fac, (pid_t)atoi(argv[2]));
}
