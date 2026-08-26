# PROJECT : 약속(藥 SLOT)

Team Name : 약속(藥SLOT)은 지켜야제

● 자동으로 약을 밀어내는 디스펜서가 아닌, 원래 포장을 유지하면서 시스템이 목표 위치를 선택하고 사람이 최종 압출을 수행하는 반자동 복약 알림이를 구현

1. 로컬 Web을 통해 휴대폰으로 접속하여 복약알림 설정을 rpi 에 기록함 ( 담당 : )
2. 기록된 설정을 atmega 와 통신하여 알림 시간에 맟게 atmega 를 제어 및 음성안내 ( 담당 : )
3. rpi 가 준 데이터를 파싱하여 atmega가 결과에 맞는 제어를 수행 ( 담당 : )

### contributors

**Lee yangbae [ two two ship ]**

* Project Leader
* Git Manager
* Create Demo Device yak_slot for Develop

**Shin Yuji [ shinyuji ]**

* atmega128
* atmega and rpi Integration

**Other_people [ who_are_you ]**

## How to manage
1. manager가 feature 브랜치에서 main 브랜치로의 PR을 확인하고 merge
2. 문제가 있다면 code에 대한 comment나 issue를 통해 feedback을 거쳐 해결

## How to Code and PR

1. 
```
  git clone [저장소명]
  cd [폴더명]
  git fetch origin
  git checkout -t origin/[본인이 담당한 브랜치 명]
```
3. commit 규칙에 맞게 code 업로드하기
   
4. 원격 저장소로 브랜치 push
   
5. GitHub에서 main 브랜치 방향으로 PR(Pull Request) 생성


## PR 규칙
* 작업 기간, 작업명, 본인 이름 포함하여 PR
  ```
  26/07/19 ~ 26/07/23 스테빙_모터_제어_수정34 이양배
  ```

## Commit 규칙
* commit message: code 제목
  ```
  git commit -m "atmega128a_uart.c"
  ```
* 모든 코드파일에는 맨 위에 이 파일이 어떤 역할 및 기능을 수행하는지 서술
