/*
 * fixture.h - 개발·테스트용 임시 현장 파일.
 * 기획서 5.1 의 plant/ 구조를 최소 내용으로 만들고, 이벤트별 파일 고장을 넣었다 고친다.
 * 실제 게임의 현장 파일과 고장 주입은 호스트 쪽 담당이고, 이 파일은 데모와 테스트만 쓴다.
 */
#ifndef FIXTURE_H
#define FIXTURE_H

/* root 를 비우고 정상 상태의 plant 트리를 만든다. root 가 이미 있으면 fixture 가 만든 폴더일 때만 지운다. */
int fixture_create(const char *root);

/* 이벤트의 파일 쪽 고장을 넣는다 (프로세스 고장은 시설 프로세스가 한다). */
int fixture_break(const char *root, int event);

/* 이벤트의 파일 쪽 고장을 정답대로 고친다. */
int fixture_fix(const char *root, int event);

/* fixture 가 만든 폴더를 지운다. */
int fixture_remove(const char *root);

#endif /* FIXTURE_H */
