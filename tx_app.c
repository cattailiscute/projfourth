#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <errno.h>

#define DEVICE_PATH "/dev/crowd_gpio0"
#define GPIO_IOCTL_SET_MODE _IOW('C', 1, int)
#define GPIO_IOCTL_SET_THRESHOLD _IOW('C', 4, int)
#define MODE_TRANSMITTER 1
#define DELAY_MS 2000

static int running = 1;

void signal_handler(int sig) {
    printf("\n송신 프로그램을 종료합니다...\n");
    running = 0;
}

void print_usage(const char *prog_name) {
    printf("사용법: %s [옵션]\n", prog_name);
    printf("옵션:\n");
    printf("  -a, --auto    자동 모드 (기본)\n");
    printf("  -m, --manual  수동 모드\n");
    printf("  -h, --help    도움말\n");
    printf("\n");
    printf("수동 모드 명령어:\n");
    printf("  enter  - 입장 신호\n");
    printf("  exit   - 퇴장 신호\n");
    printf("  status - 상태 조회\n");
    printf("  quit   - 종료\n");
}

int main(int argc, char *argv[]) {
    int auto_mode = 1;
    
    // 명령행 인수 처리
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--manual") == 0) {
            auto_mode = 0;
        } else if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--auto") == 0) {
            auto_mode = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }
    
    printf("IoT 혼잡도 시스템 - 송신 프로그램\n");
    printf("하드웨어: GPIO 17 → GPIO 26\n");
    printf("모드: %s\n", auto_mode ? "자동" : "수동");
    printf("=====================================\n");
    
    signal(SIGINT, signal_handler);
    
    int fd = open(DEVICE_PATH, O_RDWR);
    if (fd < 0) {
        perror("디바이스 열기 실패");
        printf("해결 방법:\n");
        printf("1. 드라이버 로드: sudo make load\n");
        printf("2. 권한 확인: ls -la /dev/crowd_gpio*\n");
        return 1;
    }
    
    // 송신 모드로 설정
    if (ioctl(fd, GPIO_IOCTL_SET_MODE, MODE_TRANSMITTER) < 0) {
        perror("송신 모드 설정 실패");
        close(fd);
        return 1;
    }
    
    // 임계값 설정 (50명)
    int threshold = 50;
    if (ioctl(fd, GPIO_IOCTL_SET_THRESHOLD, threshold) == 0) {
        printf("임계값 설정: %d명\n", threshold);
    }
    
    if (auto_mode) {
        // 자동 모드
        printf("자동 송신 모드 시작 (Ctrl+C로 종료)\n");
        printf("===================================\n");
        
        int count = 0;
        int people_sim = 0;  // 시뮬레이션용 인원 카운터
        
        while (running) {
            const char *cmd;
            
            // 간단한 시뮬레이션 로직
            if (people_sim == 0) {
                cmd = "ENTER";  // 아무도 없으면 입장
                people_sim++;
            } else if (people_sim >= 10) {
                cmd = "EXIT";   // 10명 이상이면 퇴장
                people_sim--;
            } else if (count % 5 == 4) {
                cmd = "STATUS"; // 5번 중 1번은 상태 조회
            } else if (count % 3 == 0) {
                cmd = "ENTER";  // 입장 확률 높게
                people_sim++;
            } else {
                cmd = "EXIT";   // 퇴장
                if (people_sim > 0) people_sim--;
            }
            
            if (write(fd, cmd, strlen(cmd)) < 0) {
                perror("신호 전송 실패");
                break;
            }
            
            printf("[%03d] %s 신호 전송 (시뮬레이션 인원: %d명)\n", 
                   ++count, cmd, people_sim);
            
            usleep(DELAY_MS * 1000);
        }
        
    } else {
        // 수동 모드
        printf("수동 송신 모드 (명령어 입력)\n");
        printf("============================\n");
        print_usage(argv[0]);
        
        char input[64];
        while (running) {
            printf("\n명령 입력> ");
            fflush(stdout);
            
            if (!fgets(input, sizeof(input), stdin)) {
                break;
            }
            
            // 개행 문자 제거
            input[strcspn(input, "\n")] = 0;
            
            if (strcmp(input, "quit") == 0) {
                break;
            } else if (strcmp(input, "enter") == 0) {
                if (write(fd, "ENTER", 5) >= 0) {
                    printf("✓ 입장 신호 전송\n");
                } else {
                    perror("전송 실패");
                }
            } else if (strcmp(input, "exit") == 0) {
                if (write(fd, "EXIT", 4) >= 0) {
                    printf("✓ 퇴장 신호 전송\n");
                } else {
                    perror("전송 실패");
                }
            } else if (strcmp(input, "status") == 0) {
                if (write(fd, "STATUS", 6) >= 0) {
                    printf("✓ 상태 조회 신호 전송\n");
                } else {
                    perror("전송 실패");
                }
            } else if (strlen(input) > 0) {
                printf("알 수 없는 명령: %s\n", input);
            }
        }
    }
    
    close(fd);
    printf("송신 프로그램 종료\n");
    return 0;
}