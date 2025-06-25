/*
 * ========================================================================
 * 라즈베리 파이 GPIO 17-26 연결 기반 IoT 혼잡도 관리 시스템
 * ========================================================================
 * 
 * 하드웨어 연결:
 * GPIO 17 (송신측) ↔ GPIO 26 (수신측)
 * GND ↔ GND
 * 
 * 파일 구성:
 * 1. crowd_driver.c - 커널 드라이버
 * 2. tx_app.c - 송신용 응용프로그램  
 * 3. rx_app.c - 수신용 응용프로그램
 * 4. Makefile - 빌드 설정
 */

/* ========================================================================
 * 1. 드라이버 코드 (crowd_driver.c)
 * ======================================================================== */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/workqueue.h>

/* 시스템 상수 */
#define DEVICE_NAME "crowd_gpio"
#define CLASS_NAME "crowd_monitor"
#define MAX_DEVICES 2

/* GPIO 핀 번호 (BCM 기준) */
#define GPIO_TX_PIN 17    /* 송신용 핀 */
#define GPIO_RX_PIN 26    /* 수신용 핀 */

/* 디바이스 모드 */
#define MODE_TRANSMITTER 1
#define MODE_RECEIVER 2

/* IOCTL 명령 */
#define GPIO_IOCTL_MAGIC 'C'
#define GPIO_IOCTL_SET_MODE _IOW(GPIO_IOCTL_MAGIC, 1, int)
#define GPIO_IOCTL_GET_COUNT _IOR(GPIO_IOCTL_MAGIC, 2, int)
#define GPIO_IOCTL_RESET_COUNT _IO(GPIO_IOCTL_MAGIC, 3)
#define GPIO_IOCTL_SET_THRESHOLD _IOW(GPIO_IOCTL_MAGIC, 4, int)

/* 디바이스 구조체 */
struct crowd_device {
    struct device *dev;
    struct cdev cdev;
    struct gpio_desc *gpio_desc;
    int device_mode;
    int current_occupancy;
    int threshold;
    bool ventilation_active;
    struct mutex device_lock;
    wait_queue_head_t read_wait;
    struct work_struct irq_work;
    int irq_num;
    bool irq_enabled;
    unsigned long total_messages;
};

/* 전역 변수 */
static dev_t dev_num_base;
static struct class *crowd_class;
static struct crowd_device *devices[MAX_DEVICES];
static int major_num;

/* ========== 헬퍼 함수들 ========== */

/* 인원 카운터 업데이트 */
static void update_occupancy(struct crowd_device *dev, int change) {
    mutex_lock(&dev->device_lock);
    
    dev->current_occupancy += change;
    if (dev->current_occupancy < 0)
        dev->current_occupancy = 0;
    
    /* 환기 시스템 제어 */
    bool should_ventilate = (dev->current_occupancy >= dev->threshold);
    if (should_ventilate != dev->ventilation_active) {
        dev->ventilation_active = should_ventilate;
        pr_info("[crowd_monitor] 환기 시스템 %s (인원: %d명, 임계값: %d명)\n",
                should_ventilate ? "작동" : "중지", 
                dev->current_occupancy, dev->threshold);
    }
    
    mutex_unlock(&dev->device_lock);
}

/* GPIO 신호 전송 (간단한 디지털 신호) */
static int send_signal(struct crowd_device *dev, int signal_type) {
    if (!dev->gpio_desc || dev->device_mode != MODE_TRANSMITTER) {
        return -EINVAL;
    }
    
    /* 신호 타입에 따른 펄스 패턴 전송 */
    switch (signal_type) {
    case 1: /* ENTER - 짧은 펄스 1개 */
        gpiod_set_value(dev->gpio_desc, 1);
        msleep(100);
        gpiod_set_value(dev->gpio_desc, 0);
        msleep(100);
        break;
        
    case 2: /* EXIT - 짧은 펄스 2개 */
        gpiod_set_value(dev->gpio_desc, 1);
        msleep(100);
        gpiod_set_value(dev->gpio_desc, 0);
        msleep(100);
        gpiod_set_value(dev->gpio_desc, 1);
        msleep(100);
        gpiod_set_value(dev->gpio_desc, 0);
        msleep(100);
        break;
        
    case 3: /* STATUS - 긴 펄스 1개 */
        gpiod_set_value(dev->gpio_desc, 1);
        msleep(300);
        gpiod_set_value(dev->gpio_desc, 0);
        msleep(100);
        break;
        
    default:
        return -EINVAL;
    }
    
    pr_info("[crowd_monitor] 신호 전송 완료: 타입 %d\n", signal_type);
    return 0;
}

/* 인터럽트 워크큐 핸들러 */
static void irq_work_handler(struct work_struct *work) {
    struct crowd_device *dev = container_of(work, struct crowd_device, irq_work);
    
    /* GPIO 상태 읽기 */
    int gpio_value = gpiod_get_value(dev->gpio_desc);
    
    if (gpio_value == 1) {  /* Rising edge - 신호 시작 */
        dev->total_messages++;
        pr_info("[crowd_monitor] 신호 수신 감지\n");
        
        /* 간단한 신호 해석 (실제로는 더 복잡한 프로토콜 필요) */
        update_occupancy(dev, 1);  /* 임시로 입장으로 처리 */
        
        /* 대기 중인 read 프로세스 깨우기 */
        wake_up_interruptible(&dev->read_wait);
    }
}

/* 인터럽트 핸들러 */
static irqreturn_t gpio_interrupt_handler(int irq, void *dev_id) {
    struct crowd_device *dev = (struct crowd_device *)dev_id;
    
    /* 워크큐에 실제 처리 작업 스케줄링 */
    schedule_work(&dev->irq_work);
    
    return IRQ_HANDLED;
}

/* ========== file_operations 함수들 ========== */

static int crowd_fops_open(struct inode *inode, struct file *filp) {
    int minor = iminor(inode);
    
    if (minor >= MAX_DEVICES || !devices[minor]) {
        return -ENODEV;
    }
    
    filp->private_data = devices[minor];
    pr_info("[crowd_monitor] 디바이스 열림 (minor: %d)\n", minor);
    
    return 0;
}

static int crowd_fops_release(struct inode *inode, struct file *filp) {
    pr_info("[crowd_monitor] 디바이스 닫힘\n");
    return 0;
}

static ssize_t crowd_fops_read(struct file *filp, char __user *buf, size_t len, loff_t *off) {
    struct crowd_device *dev = filp->private_data;
    char response[256];
    int response_len;
    
    if (!dev) return -ENODEV;
    
    /* 수신 모드에서는 GPIO 상태 변화 대기 */
    if (dev->device_mode == MODE_RECEIVER) {
        if (filp->f_flags & O_NONBLOCK) {
            /* 논블로킹 모드 - 즉시 현재 상태 반환 */
        } else {
            /* 블로킹 모드 - 신호 대기 */
            if (wait_event_interruptible(dev->read_wait, 
                                       dev->total_messages > 0)) {
                return -ERESTARTSYS;
            }
        }
        
        response_len = snprintf(response, sizeof(response), "ENTER\n");
    } else {
        /* 송신 모드에서는 현재 상태 반환 */
        mutex_lock(&dev->device_lock);
        response_len = snprintf(response, sizeof(response),
            "현재 인원: %d명\n임계값: %d명\n환기 상태: %s\n총 메시지: %lu개\n",
            dev->current_occupancy, dev->threshold,
            dev->ventilation_active ? "작동중" : "중지",
            dev->total_messages);
        mutex_unlock(&dev->device_lock);
    }
    
    if (len < response_len) {
        return -EINVAL;
    }
    
    if (copy_to_user(buf, response, response_len)) {
        return -EFAULT;
    }
    
    return response_len;
}

static ssize_t crowd_fops_write(struct file *filp, const char __user *buf, size_t len, loff_t *off) {
    struct crowd_device *dev = filp->private_data;
    char kbuf[32] = {0};
    int ret = 0;
    
    if (!dev) return -ENODEV;
    
    if (len >= sizeof(kbuf)) {
        return -EINVAL;
    }
    
    if (copy_from_user(kbuf, buf, len)) {
        return -EFAULT;
    }
    
    /* 개행 문자 제거 */
    if (len > 0 && kbuf[len - 1] == '\n') {
        kbuf[len - 1] = '\0';
    }
    
    /* 명령 처리 */
    if (strcmp(kbuf, "ENTER") == 0) {
        if (dev->device_mode == MODE_TRANSMITTER) {
            ret = send_signal(dev, 1);
        } else {
            update_occupancy(dev, 1);
        }
    } else if (strcmp(kbuf, "EXIT") == 0) {
        if (dev->device_mode == MODE_TRANSMITTER) {
            ret = send_signal(dev, 2);
        } else {
            update_occupancy(dev, -1);
        }
    } else if (strcmp(kbuf, "STATUS") == 0) {
        if (dev->device_mode == MODE_TRANSMITTER) {
            ret = send_signal(dev, 3);
        }
    } else {
        return -EINVAL;
    }
    
    return (ret == 0) ? len : ret;
}

static long crowd_fops_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
    struct crowd_device *dev = filp->private_data;
    int ret = 0;
    int value;
    
    if (!dev) return -ENODEV;
    
    switch (cmd) {
    case GPIO_IOCTL_SET_MODE:
        if (copy_from_user(&value, (int __user *)arg, sizeof(int))) {
            return -EFAULT;
        }
        
        if (value != MODE_TRANSMITTER && value != MODE_RECEIVER) {
            return -EINVAL;
        }
        
        mutex_lock(&dev->device_lock);
        dev->device_mode = value;
        
        /* GPIO 방향 설정 */
        if (value == MODE_TRANSMITTER) {
            gpiod_direction_output(dev->gpio_desc, 0);
            pr_info("[crowd_monitor] 송신 모드로 설정\n");
        } else {
            gpiod_direction_input(dev->gpio_desc);
            pr_info("[crowd_monitor] 수신 모드로 설정\n");
            
            /* 인터럽트 설정 */
            if (!dev->irq_enabled && dev->irq_num > 0) {
                ret = request_irq(dev->irq_num, gpio_interrupt_handler,
                                IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
                                "crowd_gpio_irq", dev);
                if (ret == 0) {
                    dev->irq_enabled = true;
                    pr_info("[crowd_monitor] 인터럽트 활성화\n");
                }
            }
        }
        mutex_unlock(&dev->device_lock);
        break;
        
    case GPIO_IOCTL_GET_COUNT:
        mutex_lock(&dev->device_lock);
        value = dev->current_occupancy;
        mutex_unlock(&dev->device_lock);
        
        if (copy_to_user((int __user *)arg, &value, sizeof(int))) {
            return -EFAULT;
        }
        break;
        
    case GPIO_IOCTL_RESET_COUNT:
        mutex_lock(&dev->device_lock);
        dev->current_occupancy = 0;
        dev->ventilation_active = false;
        mutex_unlock(&dev->device_lock);
        pr_info("[crowd_monitor] 카운터 리셋\n");
        break;
        
    case GPIO_IOCTL_SET_THRESHOLD:
        if (copy_from_user(&value, (int __user *)arg, sizeof(int))) {
            return -EFAULT;
        }
        
        if (value < 1 || value > 1000) {
            return -EINVAL;
        }
        
        mutex_lock(&dev->device_lock);
        dev->threshold = value;
        mutex_unlock(&dev->device_lock);
        pr_info("[crowd_monitor] 임계값 설정: %d명\n", value);
        break;
        
    default:
        return -ENOTTY;
    }
    
    return ret;
}

static const struct file_operations crowd_fops = {
    .owner = THIS_MODULE,
    .open = crowd_fops_open,
    .read = crowd_fops_read,
    .write = crowd_fops_write,
    .release = crowd_fops_release,
    .unlocked_ioctl = crowd_fops_ioctl,
};

/* ========== sysfs 속성 ========== */

static ssize_t occupancy_show(struct device *dev, struct device_attribute *attr, char *buf) {
    int minor = MINOR(dev->devt);
    if (minor >= MAX_DEVICES || !devices[minor]) return -ENODEV;
    
    return scnprintf(buf, PAGE_SIZE, "%d\n", devices[minor]->current_occupancy);
}

static ssize_t threshold_show(struct device *dev, struct device_attribute *attr, char *buf) {
    int minor = MINOR(dev->devt);
    if (minor >= MAX_DEVICES || !devices[minor]) return -ENODEV;
    
    return scnprintf(buf, PAGE_SIZE, "%d\n", devices[minor]->threshold);
}

static ssize_t threshold_store(struct device *dev, struct device_attribute *attr, 
                              const char *buf, size_t count) {
    int minor = MINOR(dev->devt);
    int value;
    
    if (minor >= MAX_DEVICES || !devices[minor]) return -ENODEV;
    
    if (kstrtoint(buf, 10, &value) < 0 || value < 1 || value > 1000) {
        return -EINVAL;
    }
    
    mutex_lock(&devices[minor]->device_lock);
    devices[minor]->threshold = value;
    mutex_unlock(&devices[minor]->device_lock);
    
    return count;
}

static ssize_t mode_show(struct device *dev, struct device_attribute *attr, char *buf) {
    int minor = MINOR(dev->devt);
    if (minor >= MAX_DEVICES || !devices[minor]) return -ENODEV;
    
    return scnprintf(buf, PAGE_SIZE, "%s\n", 
                    devices[minor]->device_mode == MODE_TRANSMITTER ? "transmitter" : "receiver");
}

static DEVICE_ATTR_RO(occupancy);
static DEVICE_ATTR_RW(threshold);
static DEVICE_ATTR_RO(mode);

/* ========== 모듈 초기화/종료 ========== */

static int create_crowd_device(int minor, int gpio_pin) {
    struct crowd_device *dev;
    int ret;
    
    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev) {
        return -ENOMEM;
    }
    
    /* GPIO 설정 */
    dev->gpio_desc = gpio_to_desc(gpio_pin);
    if (!dev->gpio_desc) {
        pr_err("[crowd_monitor] GPIO %d 획득 실패\n", gpio_pin);
        kfree(dev);
        return -ENODEV;
    }
    
    /* 기본값 설정 */
    dev->device_mode = MODE_RECEIVER;
    dev->current_occupancy = 0;
    dev->threshold = 50;
    dev->ventilation_active = false;
    dev->total_messages = 0;
    dev->irq_enabled = false;
    
    /* 동기화 객체 초기화 */
    mutex_init(&dev->device_lock);
    init_waitqueue_head(&dev->read_wait);
    INIT_WORK(&dev->irq_work, irq_work_handler);
    
    /* 인터럽트 번호 획득 */
    dev->irq_num = gpiod_to_irq(dev->gpio_desc);
    if (dev->irq_num < 0) {
        pr_warn("[crowd_monitor] GPIO %d 인터럽트 번호 획득 실패\n", gpio_pin);
        dev->irq_num = 0;
    }
    
    /* 디바이스 생성 */
    dev->dev = device_create(crowd_class, NULL, 
                           MKDEV(major_num, minor), NULL, 
                           "%s%d", DEVICE_NAME, minor);
    if (IS_ERR(dev->dev)) {
        ret = PTR_ERR(dev->dev);
        pr_err("[crowd_monitor] 디바이스 %d 생성 실패: %d\n", minor, ret);
        kfree(dev);
        return ret;
    }
    
    /* sysfs 속성 추가 */
    device_create_file(dev->dev, &dev_attr_occupancy);
    device_create_file(dev->dev, &dev_attr_threshold);
    device_create_file(dev->dev, &dev_attr_mode);
    
    devices[minor] = dev;
    pr_info("[crowd_monitor] 디바이스 %d 생성 완료 (GPIO %d)\n", minor, gpio_pin);
    
    return 0;
}

static void destroy_crowd_device(int minor) {
    struct crowd_device *dev = devices[minor];
    
    if (!dev) return;
    
    /* 인터럽트 해제 */
    if (dev->irq_enabled) {
        free_irq(dev->irq_num, dev);
    }
    
    /* 워크큐 정리 */
    cancel_work_sync(&dev->irq_work);
    
    /* sysfs 속성 제거 */
    device_remove_file(dev->dev, &dev_attr_occupancy);
    device_remove_file(dev->dev, &dev_attr_threshold);
    device_remove_file(dev->dev, &dev_attr_mode);
    
    /* 디바이스 제거 */
    device_destroy(crowd_class, MKDEV(major_num, minor));
    
    /* 메모리 해제 */
    mutex_destroy(&dev->device_lock);
    kfree(dev);
    devices[minor] = NULL;
    
    pr_info("[crowd_monitor] 디바이스 %d 제거 완료\n", minor);
}

static int __init crowd_driver_init(void) {
    int ret;
    
    pr_info("[crowd_monitor] GPIO 17-26 연결 기반 IoT 드라이버 초기화\n");
    
    /* 문자 디바이스 번호 할당 */
    ret = alloc_chrdev_region(&dev_num_base, 0, MAX_DEVICES, DEVICE_NAME);
    if (ret) {
        pr_err("[crowd_monitor] 디바이스 번호 할당 실패: %d\n", ret);
        return ret;
    }
    major_num = MAJOR(dev_num_base);
    
    /* cdev 초기화 및 등록 */
    struct cdev crowd_cdev;
    cdev_init(&crowd_cdev, &crowd_fops);
    crowd_cdev.owner = THIS_MODULE;
    ret = cdev_add(&crowd_cdev, dev_num_base, MAX_DEVICES);
    if (ret) {
        pr_err("[crowd_monitor] cdev 등록 실패: %d\n", ret);
        unregister_chrdev_region(dev_num_base, MAX_DEVICES);
        return ret;
    }
    
    /* 클래스 생성 */
    crowd_class = class_create(CLASS_NAME);
    if (IS_ERR(crowd_class)) {
        ret = PTR_ERR(crowd_class);
        pr_err("[crowd_monitor] 클래스 생성 실패: %d\n", ret);
        cdev_del(&crowd_cdev);
        unregister_chrdev_region(dev_num_base, MAX_DEVICES);
        return ret;
    }
    
    /* 디바이스 생성 */
    ret = create_crowd_device(0, GPIO_TX_PIN);  /* /dev/crowd_gpio0 - 송신용 */
    if (ret) goto err_dev0;
    
    ret = create_crowd_device(1, GPIO_RX_PIN);  /* /dev/crowd_gpio1 - 수신용 */
    if (ret) goto err_dev1;
    
    pr_info("[crowd_monitor] 드라이버 초기화 완료\n");
    pr_info("[crowd_monitor] 송신: /dev/crowd_gpio0 (GPIO %d)\n", GPIO_TX_PIN);
    pr_info("[crowd_monitor] 수신: /dev/crowd_gpio1 (GPIO %d)\n", GPIO_RX_PIN);
    
    return 0;

err_dev1:
    destroy_crowd_device(0);
err_dev0:
    class_destroy(crowd_class);
    cdev_del(&crowd_cdev);
    unregister_chrdev_region(dev_num_base, MAX_DEVICES);
    return ret;
}

static void __exit crowd_driver_exit(void) {
    pr_info("[crowd_monitor] 드라이버 종료 시작\n");
    
    /* 디바이스 제거 */
    destroy_crowd_device(0);
    destroy_crowd_device(1);
    
    /* 클래스 제거 */
    class_destroy(crowd_class);
    
    /* cdev 제거 */
    struct cdev crowd_cdev;
    cdev_del(&crowd_cdev);
    
    /* 디바이스 번호 해제 */
    unregister_chrdev_region(dev_num_base, MAX_DEVICES);
    
    pr_info("[crowd_monitor] 드라이버 종료 완료\n");
}

module_init(crowd_driver_init);
module_exit(crowd_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("IoT Team");
MODULE_DESCRIPTION("GPIO 17-26 연결 기반 IoT 혼잡도 관리 드라이버");
MODULE_VERSION("1.0");