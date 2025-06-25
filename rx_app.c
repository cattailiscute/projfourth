#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

#define DEVICE_PATH "/dev/crowd_gpio1"
#define SYSFS_OCCUPANCY "/sys/class/crowd_monitor/crowd_gpio1/occupancy"
#define SYSFS_THRESHOLD "/sys/class/crowd_monitor/crowd_gpio1/threshold"
#define GPIO_IOCTL_SET_MODE _IOW('C', 1, int)
#define MODE_RECEIVER 2
#define DELAY_MS 500

static int running = 1;

void signal_handler(int sig) {
    printf("\n수신 프로그램을 종료합니다...\n");
    running = 0;
}

void get_time_string(char *buffer, size_t size) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buffer, size, "%H:%M:%S", tm_info);
}

int read_sysfs_int(const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) return -1;
    
    int value;
    if (fscanf(file, "%d", &value) != 1) {
        fclose(file);
        return -1;
    }
    
    fclose(file);
    return value;
}

int main() {
    printf("IoT 혼잡도 시스템 - 수신 프로그램\n");
    printf("하드웨어: GPIO 26 ← GPIO 17\n");
    printf("=====================================\n");
    
    signal(SIGINT, signal_handler);
    
    int fd = open(DEVICE_PATH, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("디바이스 열기 실패");
        printf("해결 방법:\n");
        printf("1. 드라이버 로드: sudo make load\n");
        printf("2. 권한 확인: ls -la /dev/crowd_gpio*\n");
        return 1;
    }
    
    // 수신 모드로 설정
    if (ioctl(fd, GPIO_IOCTL_SET_MODE, MODE_RECEIVER) < 0) {
        perror("수신 모드 설정 실패");
        close(fd);
        return 1;
    }
    
    printf("신호 수신 대기 중... (Ctrl+C로 종료)\n");
    printf("===================================\n");
    
    char prev_msg[64] = "";
    int people_count = 0;
    int threshold = read_sysfs_int(SYSFS_THRESHOLD);
    if (threshold < 0) threshold = 50;  // 기본값
    
    printf("현재 임계값: %d명\n\n", threshold);
    
    while (running) {
        char buf[256] = {0};
        char time_str[32];
        
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        
        if (n > 0) {
            buf[n] = '\0';
            buf[strcspn(buf, "\n")] = 0;  // 개행 제거
            
            if (strlen(buf) > 0 && strcmp(buf, prev_msg) != 0) {
                get_time_string(time_str, sizeof(time_str));
                
                if (strncmp(buf, "ENTER", 5) == 0) {
                    people_count++;
                    printf("[%s] 🚪 입장 감지 - 현재 %d명", time_str, people_count);
                    
                    if (people_count >= threshold) {
                        printf(" ⚠️ 환기 필요!");
                    }
                    printf("\n");
                    
                } else if (strncmp(buf, "EXIT", 4) == 0) {
                    if (people_count > 0) people_count--;
                    printf("[%s] 🚪 퇴장 감지 - 현재 %d명\n", time_str, people_count);
                    
                } else if (strncmp(buf, "STATUS", 6) == 0) {
                    printf("[%s] 📊 상태 조회 - 현재 %d명 (임계값: %d명)\n", 
                           time_str, people_count, threshold);
                    
                } else {
                    printf("[%s] ❓ 알 수 없는 신호: %s\n", time_str, buf);
                }
                
                // sysfs에서 실제 값 읽기 (드라이버 상태와 동기화)
                int actual_count = read_sysfs_int(SYSFS_OCCUPANCY);
                if (actual_count >= 0 && actual_count != people_count) {
                    printf("    (드라이버 상태: %d명)\n", actual_count);
                    people_count = actual_count;  // 동기화
                }
                
                strcpy(prev_msg, buf);
            }
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            if (errno != EINTR) {  // SIGINT는 정상
                perror("읽기 오류");
            }
        }
        
        usleep(DELAY_MS * 1000);
    }
    
    close(fd);
    printf("수신 프로그램 종료 (최종 인원: %d명)\n", people_count);
    return 0;
}