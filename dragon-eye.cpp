// Standard include files
#include <opencv2/opencv.hpp>
#include <opencv2/videoio.hpp>

#include <opencv2/cudacodec.hpp>
#include <opencv2/cudabgsegm.hpp>
#include <opencv2/cudaobjdetect.hpp>
#include <opencv2/cudafilters.hpp>

#include <errno.h>
#include <fcntl.h> 
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <stdio.h>
#include <signal.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h> 

#include <arpa/inet.h>

using namespace cv;
using namespace std;

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>

#include <iostream>
#include <fstream>
#include <algorithm>
#include <regex>

extern "C" {
#include "jetsonGPIO/jetsonGPIO.h"
}

using std::chrono::high_resolution_clock;
using std::chrono::steady_clock;
using std::chrono::duration_cast;
using std::chrono::microseconds;

//using std::chrono::system_clock;
using std::chrono::seconds;

//#define CAMERA_1080P

#ifdef CAMERA_1080P
    #define CAMERA_WIDTH 1920
    #define CAMERA_HEIGHT 1080
    #define CAMERA_FPS 30
    #define MIN_TARGET_WIDTH 12
    #define MIN_TARGET_HEIGHT 12
    #define MAX_TARGET_WIDTH 480
    #define MAX_TARGET_HEIGHT 480
#else
    #define CAMERA_WIDTH 1280
    #define CAMERA_HEIGHT 720
    #define CAMERA_FPS 60
    #define MIN_TARGET_WIDTH 8
    #define MIN_TARGET_HEIGHT 8
    #define MAX_TARGET_WIDTH 320
    #define MAX_TARGET_HEIGHT 320
#endif

#define MAX_NUM_TARGET               3      /* Maximum targets to tracing */
#define MAX_NUM_TRIGGER              4      /* Maximum number of RF trigger after detection of cross line */
#define MAX_NUM_FRAME_MISSING_TARGET 10     /* Maximum number of frames to keep tracing lost target */

#define MIN_COURSE_LENGTH            120    /* Minimum course length of RF trigger after detection of cross line */
#define MIN_TARGET_TRACKED_COUNT     3      /* Minimum target tracked count of RF trigger after detection of cross line */

#define VIDEO_OUTPUT_DIR             "/home/gigijoe/Videos"
#define VIDEO_OUTPUT_FILE_NAME       "base"
#define VIDEO_FILE_OUTPUT_DURATION   90     /* Video file duration 90 secends */

#define STR_SIZE                  1024
#define CONFIG_FILE_DIR              "/etc/dragon-eye"

typedef enum { BASE_UNKNOWN, BASE_A, BASE_B, BASE_TIMER, BASE_ANEMOMETER } BaseType_t;

/*
*
*/

static inline bool ContoursSort(vector<cv::Point> contour1, vector<cv::Point> contour2)  
{  
    //return (contour1.size() > contour2.size()); /* Outline length */
    return (cv::contourArea(contour1) > cv::contourArea(contour2)); /* Area */
}  

/*
*
*/

class Target
{
protected:
    double m_courseLength;
    unsigned long m_frameTick;
    uint8_t m_triggerCount;

public:
    Target(Rect & roi, unsigned long frameTick) : m_courseLength(0), m_frameTick(frameTick), m_triggerCount(0) {
        m_rects.push_back(roi);
        m_points.push_back(roi.tl());
        m_frameTick = frameTick;
    }

    vector< Rect > m_rects;
    vector< Point > m_points;
    Point m_velocity;

    void Reset() {
        Rect r = m_rects.back();
        Point p = r.tl();
        m_rects.clear();
        m_points.clear();
        m_rects.push_back(r);
        m_points.push_back(p);
        m_triggerCount = 0;
        //m_frameTick =
        //m_courseLength = 0; /* TODO : Do clear this ? */
    }

    void Update(Rect & roi, unsigned long frameTick) {
        if(m_rects.size() > 0)
            m_courseLength += norm(roi.tl() - m_rects.back().tl());

        if(m_points.size() == 1) {
            m_velocity.x = (roi.tl().x - m_rects.back().tl().x);
            m_velocity.y = (roi.tl().y - m_rects.back().tl().y);
        } else if(m_points.size() > 1) {
            m_velocity.x = (m_velocity.x + (roi.tl().x - m_rects.back().tl().x)) / 2;
            m_velocity.y = (m_velocity.y + (roi.tl().y - m_rects.back().tl().y)) / 2;
        }
        m_rects.push_back(roi);
        m_points.push_back(roi.tl());
        m_frameTick = frameTick;
#if 1
        if(m_triggerCount >= MAX_NUM_TRIGGER)
            Reset();
#endif
    }

    inline double CourseLength() { return m_courseLength; }
    inline unsigned long FrameTick() { return m_frameTick; }
    inline Rect & LatestRect() { return m_rects.back(); }
    inline Point & LatestPoint() { return m_points.back(); }
    inline void Trigger() { m_triggerCount++; }
    inline uint8_t TriggerCount() { return m_triggerCount; }
    //inline Point & Velocity() { return m_velocity; }
    inline size_t TrackedCount() { return m_rects.size(); }
};

static inline bool TargetSort(Target & a, Target & b)
{
    return a.LatestRect().area() > b.LatestRect().area();
}

/*
*
*/

class Tracker
{
private:
    unsigned long m_frameTick;
    list< Target > m_targets;
    Target *m_primaryTarget;

public:
    Tracker() : m_frameTick(0), m_primaryTarget(0) {}

    void Update(vector< Rect > & roiRect) {
        const int euclidean_distance = 240;

        if(m_primaryTarget) {
            Target *t = m_primaryTarget;
            int i;
            for(i=0; i<roiRect.size(); i++) {
                Rect r = t->m_rects.back();
                if((r & roiRect[i]).area() > 0) /* Target tracked ... */
                    break;                

                unsigned long f = m_frameTick - t->FrameTick();
                r.x += t->m_velocity.x * f;
                r.y += t->m_velocity.y * f;
                if((r & roiRect[i]).area() > 0) /* Target tracked with velocity ... */
                    break;
#if 0
                if(cv::norm(r.tl()-roiRect[i].tl()) < euclidean_distance) /* Target tracked with velocity and Euclidean distance ... */
                    break;
#endif
                r = t->m_rects.back();
                if(cv::norm(r.tl()-roiRect[i].tl()) < euclidean_distance) /* Target tracked with Euclidean distance ... */
                    break;
            }
            if(i != roiRect.size()) { /* Primary Target tracked */
                t->Update(roiRect[i], m_frameTick);
                return;
            }
        }

        for(list< Target >::iterator t=m_targets.begin();t!=m_targets.end();) { /* Try to find lost targets */
            int i;
            for(i=0; i<roiRect.size(); i++) {
                Rect r = t->m_rects.back();
                if((r & roiRect[i]).area() > 0) /* Target tracked ... */
                    break;                

                unsigned long f = m_frameTick - t->FrameTick();
                r.x += t->m_velocity.x * f;
                r.y += t->m_velocity.y * f;
                if((r & roiRect[i]).area() > 0) /* Target tracked with velocity ... */
                    break;
#if 0
                if(cv::norm(r.tl()-roiRect[i].tl()) < euclidean_distance) /* Target tracked with velocity and Euclidean distance ... */
                    break;
#endif
                r = t->m_rects.back();
                if(cv::norm(r.tl()-roiRect[i].tl()) < euclidean_distance) /* Target tracked with Euclidean distance ... */
                    break;
            }
            if(i == roiRect.size()) { /* Target missing ... */
                if(m_frameTick - t->FrameTick() > MAX_NUM_FRAME_MISSING_TARGET) { /* Target still missing for over X frames */
#if 1            
                    Point p = t->m_points.back();
                    printf("lost target : %d, %d\n", p.x, p.y);
#endif
                    if(&(*t) == m_primaryTarget)
                        m_primaryTarget = 0;

                    t = m_targets.erase(t); /* Remove tracing target */
                    continue;
                }
            }
            t++;
        }

        for(int i=0; i<roiRect.size(); i++) { /* Try to find NEW target for tracking ... */
            list< Target >::iterator t;
            for(t=m_targets.begin();t!=m_targets.end();t++) {
                Rect r = t->m_rects.back();
                if((r & roiRect[i]).area() > 0) /* Next step tracked ... */
                    break;

                unsigned long f = m_frameTick - t->FrameTick();
                r.x += t->m_velocity.x * f;
                r.y += t->m_velocity.y * f;
                if((r & roiRect[i]).area() > 0) /* Next step tracked with velocity ... */
                    break;
#if 0
                if(cv::norm(r.tl()-roiRect[i].tl()) < euclidean_distance) /* Target tracked with velocity and Euclidean distance ... */
                    break;
#endif
                r = t->m_rects.back();
                if(cv::norm(r.tl()-roiRect[i].tl()) < euclidean_distance) /* Target tracked with Euclidean distance ... */                   
                    break;
            }
            if(t == m_targets.end()) { /* New target */
#if 0
                if(roiRect[i].y > 960)
                    continue;
#endif
                m_targets.push_back(Target(roiRect[i], m_frameTick));
#if 1            
                printf("new target : %d, %d\n", roiRect[i].tl().x, roiRect[i].tl().y);
#endif
            } else
                t->Update(roiRect[i], m_frameTick);
        }
        m_frameTick++;

        if(m_targets.size() > 1)
            m_targets.sort(TargetSort);

        if(m_targets.size() > 0) {
            if(m_primaryTarget == 0)
                m_primaryTarget = &m_targets.front();
        }
    }

    Target *PrimaryTarget() {
//        if(m_targets.size() == 0)
//            return 0;

//        m_targets.sort(TargetSort);
#if 0
        list< Target >::iterator t;
        for(t=m_targets.begin();t!=m_targets.end();t++) {
            if(t->ArcLength() > 320)
                return &(*t);
        }
        return 0;
#else
        return m_primaryTarget;
#endif
    }
};

/*
*
*/

class FrameQueue
{
public:
    struct cancelled {};

public:
    FrameQueue() : cancelled_(false) {}

    void push(Mat const & image);
    Mat pop();

    void cancel();
    bool isCancelled() { return cancelled_; }
    void reset();

private:
    std::queue<cv::Mat> queue_;
    std::mutex mutex_;
    std::condition_variable cond_;
    bool cancelled_;
};

void FrameQueue::cancel()
{
    std::unique_lock<std::mutex> mlock(mutex_);
    cancelled_ = true;
    cond_.notify_all();
}

void FrameQueue::push(cv::Mat const & image)
{
    while(queue_.size() >= 3) /* Prevent memory overflow ... */
        return; /* Drop frame */

    std::unique_lock<std::mutex> mlock(mutex_);
    queue_.push(image);
    cond_.notify_one();
}

Mat FrameQueue::pop()
{
    std::unique_lock<std::mutex> mlock(mutex_);

    while (queue_.empty()) {
        if (cancelled_)
            throw cancelled();
        cond_.wait(mlock);
        if (cancelled_)
            throw cancelled();
    }
    Mat image(queue_.front());
    queue_.pop();
    return image;
}

void FrameQueue::reset()
{
    cancelled_ = false;
}

/*
*
*/

static FrameQueue videoOutputQueue;

void VideoOutputThread(BaseType_t baseType, bool isVideoOutputScreen, bool isVideoOutputFile, bool isVideoOutputRTP, const char *rtpRemoteHost, uint16_t rtpRemotePort, int width, int height)
{    
    Size videoSize = Size((int)width,(int)height);
    char gstStr[STR_SIZE];

    VideoWriter outFile;
    VideoWriter outScreen;
    VideoWriter outRTP;

    char filePath[64];
    int videoOutoutIndex = 0;
    while(videoOutoutIndex < 1000) {
        snprintf(filePath, 64, "%s/%s%c%03d.mkv", VIDEO_OUTPUT_DIR, VIDEO_OUTPUT_FILE_NAME, (baseType == BASE_A) ? 'A' : 'B', videoOutoutIndex);
        FILE *fp = fopen(filePath, "rb");
        if(fp) { /* file exist ... */
            fclose(fp);
            videoOutoutIndex++;
        } else
            break; /* File doesn't exist. OK */
    }

    if(isVideoOutputFile) {
    /* Countclockwise rote 90 degree - nvvidconv flip-method=1 */
#if 1
        snprintf(gstStr, STR_SIZE, "appsrc ! video/x-raw, format=(string)BGR ! \
                   videoconvert ! video/x-raw, format=(string)I420, framerate=(fraction)%d/1 ! \
                   nvvidconv flip-method=1 ! omxh265enc preset-level=3 bitrate=8000000 ! matroskamux ! filesink location=%s/%s%c%03d.mkv ", 
            30, VIDEO_OUTPUT_DIR, VIDEO_OUTPUT_FILE_NAME, (baseType == BASE_A) ? 'A' : 'B', videoOutoutIndex);
#else /* NOT work, due to tee */
        snprintf(gstStr, STR_SIZE, "appsrc ! video/x-raw, format=(string)BGR ! \
videoconvert ! video/x-raw, format=(string)I420, framerate=(fraction)%d/1 ! \
nvvidconv flip-method=1 ! omxh265enc control-rate=2 bitrate=4000000 ! \
tee name=t \
t. ! queue ! matroskamux ! filesink location=%s/%s%c%03d.mkv  \
t. ! queue ! video/x-h265, stream-format=byte-stream ! h265parse ! rtph265pay mtu=1400 ! \
udpsink host=224.1.1.1 port=5000 auto-multicast=true sync=false async=false ",
            30, VIDEO_OUTPUT_DIR, VIDEO_OUTPUT_FILE_NAME, (baseType == BASE_A) ? 'A' : 'B', videoOutoutIndex);
#endif
        outFile.open(gstStr, VideoWriter::fourcc('X', '2', '6', '4'), 30, videoSize);
        cout << endl;
        cout << gstStr << endl;
        cout << endl;
        cout << "*** Start record video ***" << endl;
    }

    if(isVideoOutputScreen) {
        snprintf(gstStr, STR_SIZE, "appsrc ! video/x-raw, format=(string)BGR ! \
videoconvert ! video/x-raw, format=(string)I420, framerate=(fraction)%d/1 ! \
nvvidconv ! video/x-raw(memory:NVMM) ! \
nvoverlaysink sync=false ", 30);
        outScreen.open(gstStr, VideoWriter::fourcc('I', '4', '2', '0'), 30, Size(CAMERA_WIDTH, CAMERA_HEIGHT));
        cout << endl;
        cout << gstStr << endl;
        cout << endl;
        cout << "*** Start display video ***" << endl;
    }

    if(isVideoOutputRTP && rtpRemoteHost) {
#undef MULTICAST_RTP
#ifdef MULTICAST_RTP /* Multicast RTP does NOT work with wifi due to low speed ... */
        snprintf(gstStr, STR_SIZE, "appsrc ! video/x-raw, format=(string)BGR ! \
videoconvert ! video/x-raw, format=(string)I420, framerate=(fraction)%d/1 ! \
nvvidconv flip-method=1 ! omxh265enc control-rate=2 bitrate=4000000 ! video/x-h265, stream-format=byte-stream ! \
h265parse ! rtph265pay mtu=1400 ! udpsink host=224.1.1.1 port=%u auto-multicast=true sync=false async=false ",
            30, rtpRemotePort);
#else
        snprintf(gstStr, STR_SIZE, "appsrc ! video/x-raw, format=(string)BGR ! \
videoconvert ! video/x-raw, format=(string)I420, framerate=(fraction)%d/1 ! \
nvvidconv flip-method=1 ! omxh265enc control-rate=2 bitrate=4000000 ! video/x-h265, stream-format=byte-stream ! \
h265parse ! rtph265pay mtu=1400 ! udpsink host=%s port=%u sync=false async=false ",
            30, rtpRemoteHost, rtpRemotePort);
#endif
        outRTP.open(gstStr, VideoWriter::fourcc('X', '2', '6', '4'), 30, Size(CAMERA_WIDTH, CAMERA_HEIGHT));
        cout << endl;
        cout << gstStr << endl;
        cout << endl;
        cout << "*** Start RTP video ***" << endl;        
    }

    videoOutputQueue.reset();

    steady_clock::time_point t1 = steady_clock::now();

    try {
        while(1) {
            Mat frame = videoOutputQueue.pop();
            if(isVideoOutputFile)
                outFile.write(frame);
            if(isVideoOutputScreen)
                outScreen.write(frame);
            if(isVideoOutputRTP)
                outRTP.write(frame);
            
            steady_clock::time_point t2 = steady_clock::now();
            double secs(static_cast<double>(duration_cast<seconds>(t2 - t1).count()));

            if(isVideoOutputFile && secs >= VIDEO_FILE_OUTPUT_DURATION)
                videoOutputQueue.cancel(); /* Reach duration limit, stop record video */
        }
    } catch (FrameQueue::cancelled & /*e*/) {
        // Nothing more to process, we're done
        if(isVideoOutputFile) {
            cout << endl;
            cout << "*** Stop record video ***" << endl;
            outFile.release();
        }
        if(isVideoOutputScreen) {
            cout << endl;
            cout << "*** Stop display video ***" << endl;
            outScreen.release();
        }
        if(isVideoOutputRTP) {
            cout << endl;
            cout << "*** Stop RTP video ***" << endl;
            outRTP.release();
        }

    }    
}

/*
*
*/

static bool IsValidateIpAddress(const string & ipAddress)
{
    struct sockaddr_in sa;
    int result = inet_pton(AF_INET, ipAddress.c_str(), &(sa.sin_addr));
    return result != 0;
}

static size_t ParseConfigFile(char *file, vector<pair<string, string> > & cfg)
{
    ifstream input(file);
    if(input.is_open()) {
        for(string line; getline( input, line ); ) {
            //line.erase(remove_if(line.begin(), line.end(), ::isspace), line.end());
            line = std::regex_replace(line, std::regex("^ +| +$|( ) +"), "$1"); /* Strip leading & tail space */
            if(line[0] == '#' || line.empty())
                    continue;
            
            auto delimiterPos = line.find("=");
            auto name = line.substr(0, delimiterPos);
            auto value = line.substr(delimiterPos + 1);

            name = std::regex_replace(name, std::regex(" +$"), ""); /* Remove tail space */
            value = std::regex_replace(value, std::regex("^ +"), ""); /* Remove leading space */

            //cfg.insert(pair<string, string>(name, value));
            cfg.push_back(make_pair(name, value));
        }
        input.close();
    }
    return cfg.size();
}

/*
*
*/

class F3xBase {
private:
    int m_ttyFd, m_udpSocket;
    BaseType_t m_baseType;
    bool m_isBaseRemoteControl;

    jetsonNanoGPIONumber m_redLED, m_greenLED, m_blueLED, m_relay;
    jetsonNanoGPIONumber m_videoOutputScreenSwitch, m_videoOutputFileSwitch;
    jetsonNanoGPIONumber m_baseSwitch, m_videoOutputResultSwitch, m_pushButton;

    bool m_isBaseHwSwitch;
    bool m_isVideoOutputScreen;
    bool m_isVideoOutputFile;
    bool m_isVideoOutputRTP;
    bool m_isVideoOutputResult;

    string m_udpRemoteHost;
    uint16_t m_udpRemotePort;
    string m_rtpRemoteHost;
    uint16_t m_rtpRemotePort;

    int SetupTTY(int fd, int speed, int parity) {
        struct termios tty;
        memset (&tty, 0, sizeof tty);
        if(tcgetattr (fd, &tty) != 0) {
            printf ("error %d from tcgetattr\n", errno);
            return -1;
        }

        cfsetospeed (&tty, speed);
        cfsetispeed (&tty, speed);
        cfmakeraw(&tty); /* RAW mode */

        tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;     // 8-bit chars
        // disable IGNBRK for mismatched speed tests; otherwise receive break
        // as \000 chars
        tty.c_iflag &= ~IGNBRK;         // disable break processing
        tty.c_lflag = 0;                // no signaling chars, no echo,
                                        // no canonical processing
        tty.c_oflag = 0;                // no remapping, no delays
        tty.c_cc[VMIN]  = 0;            // read doesn't block
        //tty.c_cc[VTIME] = 1;            // 0.1 seconds read timeout
        tty.c_cc[VTIME] = 0;            // Non block read

        tty.c_iflag &= ~(IXON | IXOFF | IXANY); // shut off xon/xoff ctrl

        tty.c_cflag |= (CLOCAL | CREAD);// ignore modem controls,
                                        // enable reading
        tty.c_cflag &= ~(PARENB | PARODD);      // shut off parity
        tty.c_cflag |= parity;
        tty.c_cflag &= ~CSTOPB;
        tty.c_cflag &= ~CRTSCTS;

        if(tcsetattr (fd, TCSANOW, &tty) != 0) {
            printf ("error %d from tcsetattr\n", errno);
            return -1;
        }
        return 0;
    }

public:
    F3xBase() : m_ttyFd(0), m_udpSocket(0), m_baseType(BASE_A), m_isBaseRemoteControl(false),
        m_redLED(gpio16), m_greenLED(gpio17), m_blueLED(gpio50), m_relay(gpio51),
        m_videoOutputScreenSwitch(gpio19), m_videoOutputFileSwitch(gpio20),
        m_baseSwitch(gpio12), m_videoOutputResultSwitch(gpio13), m_pushButton(gpio18),
        m_isBaseHwSwitch(false), 
        m_isVideoOutputScreen(false), 
        m_isVideoOutputFile(false), 
        m_isVideoOutputRTP(false), 
        m_isVideoOutputResult(false),
        m_udpRemotePort(4999), m_rtpRemotePort(5000) {
    }

    void SetupGPIO() {
        /* 
        * Do enable GPIO by /etc/profile.d/export-gpio.sh 
        */

        /* Output */
        gpioExport(m_redLED);
        gpioExport(m_greenLED);
        gpioExport(m_blueLED);
        gpioExport(m_relay);

        gpioSetDirection(m_redLED, outputPin); /* Flash during object detected */
        gpioSetValue(m_redLED, off);

        gpioSetDirection(m_greenLED, outputPin); /* Flash during frames */
        gpioSetValue(m_greenLED, on);

        gpioSetDirection(m_blueLED, outputPin); /* Flash during file save */
        gpioSetValue(m_blueLED, off);

        gpioSetDirection(m_relay, outputPin); /* */
        gpioSetValue(m_relay, off);

        /* Input */
        gpioExport(m_videoOutputScreenSwitch);
        gpioExport(m_videoOutputFileSwitch);

        gpioExport(m_baseSwitch);
        gpioExport(m_videoOutputResultSwitch);
        gpioExport(m_pushButton);

        gpioSetDirection(m_videoOutputScreenSwitch, inputPin); /* Base A / B */
        gpioSetDirection(m_videoOutputFileSwitch, inputPin); /* Video output on / off */

        gpioSetDirection(m_baseSwitch, inputPin);
        gpioSetDirection(m_videoOutputResultSwitch, inputPin);
        gpioSetDirection(m_pushButton, inputPin); /* Pause / Restart */

    #if 0
        gpioSetEdge(m_pushButton, "rising");
        int gfd = gpioOpen(m_pushButton);
        if(gfd) {
            char v;
            struct pollfd p;
            p.fd = fd;
            poll(&p, 1, -1); /* Discard first IRQ */
            read(fd, &v, 1);
            while(1) {
                poll(&p, 1, -1);
                if((p.revents & POLLPRI) == POLLPRI) {
                    lseek(fd, 0, SEEK_SET);
                    read(fd, &v, 1);
                    // printf("Interrup GPIO value : %c\n", v);
                }
            }
            gpioCloae(gfd);
        }
    #endif
    }

    int OpenTty() { /* Try ttyUSB0 first then ttyTHS1 */
        const char *ttyUSB0 = "/dev/ttyUSB0";
        const char *ttyTHS1 = "/dev/ttyTHS1";

        m_ttyFd = open (ttyUSB0, O_RDWR | O_NOCTTY | O_SYNC);
        if(m_ttyFd) {
            SetupTTY(m_ttyFd, B9600, 0);  // set speed to 9600 bps, 8n1 (no parity)
            printf("Open %s successful ...\n", ttyUSB0);
        } else {
            printf("Error %d opening %s: %s\n", errno, ttyUSB0, strerror (errno));
            m_ttyFd = open (ttyTHS1, O_RDWR | O_NOCTTY | O_SYNC);
            if(m_ttyFd) {
                SetupTTY(m_ttyFd, B9600, 0);  // set speed to 9600 bps, 8n1 (no parity)
                printf("Open %s successful ...\n", ttyTHS1);
            }
        }

        return m_ttyFd;
    }

    int OpenUdpSocket() {
        int sockfd;
        struct sockaddr_in addr; 

        if(m_udpRemoteHost.empty()) {
            printf("Invalid UDP host !!!\n");
            return 0;
        }

        sockfd = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if(sockfd < 0) {
            printf("Error open UDP socket !!!\n");
            return 0;
        }

        memset((char*) &(addr),0, sizeof((addr)));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(m_udpRemotePort);

        if(bind(sockfd,(struct sockaddr*)&addr, sizeof(addr)) != 0) {
            switch(errno) {
            case 0:
                printf("Could not bind socket\n");
            case EADDRINUSE:
                printf("Port %u for receiving UDP is in use\n", m_udpRemotePort);
                break;
            case EADDRNOTAVAIL:
                printf("Cannot assign requested address\n");
                break;
            default:
                printf("Could not bind UDP receive port : Error %s\n", strerror(errno));
                break;            
            }
            return 0;
        }

        m_udpSocket = sockfd;

        printf("Open UDP socket successful ...\n");

        return m_udpSocket;
    }

    void CloseTty() {
        if(m_ttyFd)
            close(m_ttyFd);
        m_ttyFd = 0;
    }

    void CloseUdpSocket() {
        if(m_udpSocket)
            close(m_udpSocket);
        m_udpSocket = 0;
    }

    void TriggerTty(bool newTrigger) {
        static uint8_t serNo = 0x3f;
        uint8_t data[1];

        if(!m_ttyFd)
            return;

        if(newTrigger) { /* It's NEW trigger */
            if(++serNo > 0x3f)
                serNo = 0;
        }

        if(m_baseType == BASE_A) {
            data[0] = (serNo & 0x3f);
            printf("BASE_A[%d]\r\n", serNo);
        } else if(m_baseType == BASE_B) {
            data[0] = (serNo & 0x3f) | 0x40;
            printf("BASE_B[%d]\r\n", serNo);
        }        
        write(m_ttyFd, data, 1);
    }

    size_t WriteUdpSocket(uint8_t *data, size_t size) {
        if(!m_udpSocket)
            return 0;
        struct sockaddr_in to;
        int toLen = sizeof(to);
        memset(&to, 0, toLen);
        
        struct hostent *server;
        server = gethostbyname(m_udpRemoteHost.c_str());

        to.sin_family = AF_INET;
        to.sin_port = htons(m_udpRemotePort);
        to.sin_addr = *((struct in_addr *)server->h_addr);
        
        //printf("Write to %s:%u ...\n", m_udpRemoteHost.c_str(), m_udpRemotePort);
        
        int s = sendto(m_udpSocket, data, size, 0,(struct sockaddr*)&to, toLen);

        return s;
    }

    void TriggerUdpSocket(bool newTrigger) {
        static uint8_t serNo = 0x3f;
        uint8_t data[1];

        if(!m_udpSocket)
            return;

        if(newTrigger) { /* It's NEW trigger */
            if(++serNo > 0x3f)
                serNo = 0;
        }
        if(m_baseType == BASE_A) {
            data[0] = (serNo & 0x3f);
            printf("BASE_A[%d]\r\n", serNo);            
        } else if(m_baseType == BASE_B) {
            data[0] = (serNo & 0x3f) | 0x40;
            printf("BASE_B[%d]\r\n", serNo);
        }        
        WriteUdpSocket(data, 1);
    }

    void UpdateHwSwitch() {
        cout << endl;
        cout << "### H/W switch" << endl; 

        unsigned int gv;
        gpioGetValue(m_baseSwitch, &gv);

        if(gv == 0) 
            m_baseType = BASE_A;
        else
            m_baseType = BASE_B;

        printf("\n### BASE %c\n", gv ? 'B' : 'A');

        gpioGetValue(m_videoOutputScreenSwitch, &gv);
        if(gv == 0) /* pull low */
            m_isVideoOutputScreen = true;
        else                    
            m_isVideoOutputScreen = false;

        printf("### Video output screen : %s\n", m_isVideoOutputScreen ? "Enable" : "Disable");

        gpioGetValue(m_videoOutputFileSwitch, &gv);
        if(gv == 0)
            m_isVideoOutputFile = true;
        else
            m_isVideoOutputFile = false;

        printf("### Video output file : %s\n", m_isVideoOutputFile ? "Enable" : "Disable");

        gpioGetValue(m_videoOutputResultSwitch, &gv);
        if(gv == 0)
            m_isVideoOutputResult = true;
        else
            m_isVideoOutputResult = false;                

        printf("### Video output result : %s\n", m_isVideoOutputResult ? "Enable" : "Disable");
    } 

    void UpdateSystemConfig() {
        char fn[STR_SIZE];

        snprintf(fn, STR_SIZE, "%s/system.config", CONFIG_FILE_DIR);
        vector<pair<string, string> > cfg;
        ParseConfigFile(fn, cfg);

        cout << endl;
        cout << "### System config" << endl; 
        vector<pair<string, string> >::iterator it;
        for (it=cfg.begin(); it!=cfg.end(); it++) {
            cout << it->first << " = " << it->second << endl;
            
            if(it->first == "base.type") {
                if(it->second == "A")
                    m_baseType = BASE_A;
                else if(it->second == "B")
                    m_baseType = BASE_B;
                else
                    m_baseType = BASE_UNKNOWN;
            } else if(it->first == "base.remote.control") { /* Start or Pasue can be remote control */
                if(it->second == "yes" || it->second == "1")
                    m_isBaseRemoteControl = true;
                else
                    m_isBaseRemoteControl = false;
            } else if(it->first == "base.hwswitch") {
                if(it->second == "yes" || it->second == "1")
                    m_isBaseHwSwitch = true;
                else
                    m_isBaseHwSwitch = false;
            } else if(it->first == "base.trigger.reset") { /* triggle then reset target */
            } else if(it->first == "base.udp.remote.host") {
                if(IsValidateIpAddress(it->second))
                    m_udpRemoteHost = it->second;
            } else if(it->first == "base.udp.remote.port") {
                string & s = it->second;
                if(::all_of(s.begin(), s.end(), ::isdigit))
                    m_udpRemotePort = stoi(s);
                else
                    cout << "Invalid " << it->first << "=" << s << endl;
            } else if(it->first == "base.rtp.remote.host") {
                if(IsValidateIpAddress(it->second))
                    m_rtpRemoteHost = it->second;
            } else if(it->first == "base.rtp.remote.port") {
                string & s = it->second;
                if(::all_of(s.begin(), s.end(), ::isdigit))
                    m_rtpRemotePort = stoi(s);
                else
                    cout << "Invalid " << it->first << "=" << s << endl;
            } else if(it->first == "video.output.screen") {
                if(it->second == "yes" || it->second == "1")
                    m_isVideoOutputScreen = true;
                else
                    m_isVideoOutputScreen = false;
            } else if(it->first == "video.output.file") {
                if(it->second == "yes" || it->second == "1")
                    m_isVideoOutputFile = true;
                else
                    m_isVideoOutputFile = false;
            } else if(it->first == "video.output.rtp") {
                if(it->second == "yes" || it->second == "1")
                    m_isVideoOutputRTP = true;
                else
                    m_isVideoOutputRTP = false;
            } else if(it->first == "video.output.result") {
                if(it->second == "yes" || it->second == "1")
                    m_isVideoOutputResult = true;
                else
                    m_isVideoOutputResult = false;
            }
        }
    }

    inline BaseType_t BaseType() const {
        return m_baseType;
    }

    inline bool IsBaseRemoteControl() const {
        return m_isBaseRemoteControl;
    }

    inline bool IsBaseHwSwitch() {
        return m_isBaseHwSwitch;        
    }

    inline bool IsVideoOutputScreen() const {
        return m_isVideoOutputScreen;
    }

    inline bool IsVideoOutputFile() const {
        return m_isVideoOutputFile;
    }

    inline bool IsVideoOutputRTP() const {
        return m_isVideoOutputRTP;
    }

    const char *RtpRemoteHost() {
        if(m_rtpRemoteHost.empty())
            return 0;
        return m_rtpRemoteHost.c_str();
    }

    inline uint16_t RtpRemotePort() {
        return m_rtpRemotePort;
    }

    const char *UdpRemoteHost() {
        if(m_udpRemoteHost.empty())
            return 0;
        return m_udpRemoteHost.c_str();
    }

    inline uint16_t UdpRemotePort() {
        return m_udpRemotePort;
    }

    inline bool IsVideoOutput() const {
        return (m_isVideoOutputScreen || m_isVideoOutputFile || m_isVideoOutputRTP);
    }

    inline bool IsVideoOutputResult() const {
        return m_isVideoOutputResult;
    }

    int ReadTty(uint8_t *data, size_t size) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(m_ttyFd, &rfds);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 0;

        if(data == 0 || size == 0)
            return 0;
        if(select(m_ttyFd+1, &rfds, NULL, NULL, &tv) > 0) {
            int r = read(m_ttyFd, data, size); /* Receive trigger from f3f timer */
            if(r == size)
                return 1;
        }
        return 0;
    }

    size_t ReadUdpSocket(uint8_t *data, size_t size) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(m_udpSocket, &rfds);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 0;

        if(data == 0 || size == 0)
            return 0;

        if(select(m_udpSocket+1, &rfds, NULL, NULL, &tv) > 0) {
            struct sockaddr_in from;
            int fromLen = sizeof(from);
            size_t originalSize = size;

            int r = recvfrom(m_udpSocket,
                   data,
                   originalSize,
                   0,
                   (struct sockaddr *)&from,
                   (socklen_t*)&fromLen);

            if(r > 0)
                return r;
            else if(r == -1) {
                switch(errno) {
                   case ENOTSOCK:
                      printf("Error fd not a socket\n");
                      break;
                   case ECONNRESET:
                      printf("Error connection reset - host not reachable\n");
                      break;              
                   default:
                      printf("Socket Error : %d\n", errno);
                      break;
                }
            }
        }
        return 0;
    }

    void RedLed(pinValues onOff) {
        gpioSetValue(m_redLED, onOff);
    }

    void GreenLed(pinValues onOff) {
        gpioSetValue(m_greenLED, onOff);
    }

    void BlueLed(pinValues onOff) {
        gpioSetValue(m_blueLED, onOff);
    }

    void Relay(pinValues onOff) {
        gpioSetValue(m_relay, onOff);
    }

    unsigned int PushButton() {
        unsigned int gv;
        gpioGetValue(m_pushButton, &gv);
        return gv;
    }
};

static F3xBase f3xBase; 

/*
*
*/

class Camera {
private:
    VideoCapture cap;
    char gstStr[STR_SIZE];

public:
    int wbmode = 0;
    int tnr_mode = 1;
    int tnr_strength = -1;
    int ee_mode = 1;
    int ee_strength = -1;
    string gainrange; /* Default null */
    string ispdigitalgainrange; /* Default null */
    string exposuretimerange; /* Default null */
    int exposurecompensation = 0;
    int exposurethreshold = 255;
//    int width, height;

    Camera() : wbmode(0), tnr_mode(-1), tnr_strength(-1), ee_mode(1), ee_strength(-1), 
        gainrange("1 16"), ispdigitalgainrange("1 8"), exposuretimerange("5000000 10000000"),
        exposurecompensation(0) {
    }

    bool Open() {
        if(cap.isOpened())
            return true;
/* Reference : nvarguscamerasrc.txt */
/* export GST_DEBUG=2 to show debug message */
#if 0
    snprintf(gstStr, STR_SIZE, "nvarguscamerasrc wbmode=0 tnr-mode=2 tnr-strength=1 ee-mode=1 ee-strength=0 gainrange=\"1 16\" ispdigitalgainrange=\"1 8\" exposuretimerange=\"5000000 20000000\" exposurecompensation=0 ! \
video/x-raw(memory:NVMM), width=(int)%d, height=(int)%d, format=(string)NV12, framerate=(fraction)%d/1 ! \
nvvidconv flip-method=2 ! video/x-raw, format=(string)BGRx ! videoconvert ! video/x-raw, format=(string)BGR ! appsink max-buffers=1 drop=true ", 
        CAMERA_WIDTH, CAMERA_HEIGHT, CAMERA_FPS);
#else
        snprintf(gstStr, STR_SIZE, "nvarguscamerasrc wbmode=%d tnr-mode=%d tnr-strength=%d ee-mode=%d ee-strength=%d gainrange=%s ispdigitalgainrange=%s exposuretimerange=%s exposurecompensation=%d ! \
video/x-raw(memory:NVMM), width=(int)%d, height=(int)%d, format=(string)NV12, framerate=(fraction)%d/1 ! \
nvvidconv flip-method=2 ! video/x-raw, format=(string)BGRx ! videoconvert ! video/x-raw, format=(string)BGR ! appsink max-buffers=1 drop=true ", 
            wbmode, tnr_mode, tnr_strength, ee_mode, ee_strength, gainrange.c_str(), ispdigitalgainrange.c_str(), exposuretimerange.c_str(), exposurecompensation,
            CAMERA_WIDTH, CAMERA_HEIGHT, CAMERA_FPS);
#endif
/*
        snprintf(gstStr, STR_SIZE, "v4l2src device=/dev/video1 ! \
video/x-raw(memory:NVMM), width=(int)%d, height=(int)%d, format=(string)NV12, framerate=(fraction)%d/1 ! \
nvvidconv flip-method=2 ! video/x-raw, format=(string)BGRx ! videoconvert ! video/x-raw, format=(string)BGR ! appsink max-buffers=1 drop=true ", 
            CAMERA_WIDTH, CAMERA_HEIGHT, CAMERA_FPS);
*/
        cout << endl;
        cout << gstStr << endl;
        cout << endl;

        if(!cap.open(gstStr, cv::CAP_GSTREAMER)) {
            cout << endl;
            cout << "!!! Could not open video" << endl;
            return false;
        }

        return true;
    }

    void Close() {
        if(cap.isOpened())
            cap.release();
    }

    inline bool Read(OutputArray & a) { return cap.read(a); }

    void UpdateCameraConfig() {
        char str[STR_SIZE];
        snprintf(str, STR_SIZE, "%s/camera.config", CONFIG_FILE_DIR);
        vector<pair<string, string> > cfg;
        ParseConfigFile(str, cfg);

        cout << endl;
        cout << "### Camera config" << endl; 
        vector<pair<string, string> >::iterator it;
        for (it=cfg.begin(); it!=cfg.end(); it++) {
            cout << it->first << " = " << it->second << endl;
            if(it->first == "wbmode")
                wbmode = stoi(it->second);
            else if(it->first == "tnr-mode")
                tnr_mode = stoi(it->second);
            else if(it->first == "tnr-strength")
                tnr_strength = stoi(it->second);
            else if(it->first == "ee-mode")
                ee_mode = stoi(it->second);
            else if(it->first == "ee-strength")
                ee_strength = stoi(it->second);
            else if(it->first == "gainrange")
                gainrange = it->second;
            else if(it->first == "ispdigitalgainrange")
                ispdigitalgainrange = it->second;
            else if(it->first == "exposuretimerange")
                exposuretimerange = it->second;
            else if(it->first == "exposurecompensation")
                exposurecompensation = stoi(it->second);
            else if(it->first == "exposurethreshold")
                exposurethreshold = stoi(it->second);
        }
        cout << endl;
    }

    bool UpdateExposure() {
        const char *exposureTimeRange[5] = {
            "\"5000000 10000000\"",
            "\"3000000 8000000\"",
            "\"1000000 5000000\"",
            "\"500000 2000000\"",
            "\"250000 1000000\""
        };

        if(cap.isOpened())
            cap.release();

        vector<pair<string, float> > exposure_brightness;

        for(int i=0;i<5;i++) {
            snprintf(gstStr, STR_SIZE, "nvarguscamerasrc wbmode=%d tnr-mode=%d tnr-strength=%d ee-mode=%d ee-strength=%d gainrange=%s ispdigitalgainrange=%s exposuretimerange=%s exposurecompensation=%d ! \
video/x-raw(memory:NVMM), width=(int)%d, height=(int)%d, format=(string)NV12, framerate=(fraction)%d/1 ! \
nvvidconv flip-method=2 ! video/x-raw, format=(string)BGRx ! videoconvert ! video/x-raw, format=(string)BGR ! appsink max-buffers=1 drop=true ", 
                wbmode, tnr_mode, tnr_strength, ee_mode, ee_strength, gainrange.c_str(), ispdigitalgainrange.c_str(), exposureTimeRange[i], exposurecompensation,
                CAMERA_WIDTH, CAMERA_HEIGHT, CAMERA_FPS);

            cout << endl;
            cout << gstStr << endl;
            cout << endl;

            if(!cap.open(gstStr, cv::CAP_GSTREAMER)) {
                cout << endl;
                cout << "!!! Could not open video" << endl;
                return false;
            }

            Mat capFrame;
            int meanCount = 30;
            float meanValue = 0;
            while(meanCount-- > 0) {
                cap.read(capFrame);
                Mat grayFrame;
                cvtColor(capFrame, grayFrame, COLOR_BGR2GRAY);
                Scalar v = mean(grayFrame);
                meanValue += v.val[0];
            }
            meanValue /= 30;
            
            cap.release();

            //cout << "Exposure time range - " << exposureTimeRange[i] << " : Brightness - " << meanValue << endl;
            exposure_brightness.push_back(make_pair(exposureTimeRange[i], meanValue));
        }

        vector<pair<string, float> >::iterator it;
        for (it=exposure_brightness.begin(); it!=exposure_brightness.end(); it++) {
            cout << "Exposure time range - " << it->first << " : Brightness - " << it->second << endl;
        }

        for (it=exposure_brightness.begin(); it!=exposure_brightness.end(); it++) {
            if(it->second <= exposurethreshold) {
                exposuretimerange = it->first;
                cout << endl;
                cout << "### Set exposure time range - " << it->first <<  endl;
                break;
            }
        }

        return true;
    }
};

static Camera camera; 

/*
*
*/

static bool bShutdown = false;
static bool bPause = true;

/*
*
*/

static thread voThread;
static thread udpThread;

static void OnPushButton(F3xBase & fb) 
{
    bPause = !bPause;

    if(bPause) {
        cout << endl;
        cout << "*** Object tracking stoped ***" << endl;

        camera.UpdateExposure();
        //camera.Close();
        
        if(fb.IsVideoOutput()) {
            videoOutputQueue.cancel();
            voThread.join();
        }
    } else { /* Restart, record new video */
        fb.UpdateSystemConfig();
        if(fb.IsBaseHwSwitch()) /* Overwrite config by HW switch */
            fb.UpdateHwSwitch();

        cout << endl;
        cout << "*** Object tracking started ***" << endl;

        camera.Open();

        if(fb.IsVideoOutput())
            voThread = thread(&VideoOutputThread, fb.BaseType(), fb.IsVideoOutputScreen(), fb.IsVideoOutputFile(), fb.IsVideoOutputRTP(), fb.RtpRemoteHost(), fb.RtpRemotePort(), CAMERA_WIDTH, CAMERA_HEIGHT);
    }
}

static void contour_moving_object(Mat & foregroundMask, vector<Rect> & roiRect, int y_offset = 0)
{
    uint32_t num_target = 0;

    vector< vector<Point> > contours;
    vector< Vec4i > hierarchy;
//  findContours(foregroundMask, contours, hierarchy, RETR_TREE, CHAIN_APPROX_NONE);
    findContours(foregroundMask, contours, hierarchy, RETR_EXTERNAL, CHAIN_APPROX_SIMPLE);
    sort(contours.begin(), contours.end(), ContoursSort); /* Contours sort by area, controus[0] is largest */

    vector<Rect> boundRect( contours.size() );
    for(int i=0; i<contours.size(); i++) {
        approxPolyDP( Mat(contours[i]), contours[i], 3, true );
        boundRect[i] = boundingRect( Mat(contours[i]) );
        //drawContours(contoursImg, contours, i, color, 2, 8, hierarchy);
        if(boundRect[i].width > MIN_TARGET_WIDTH && 
            boundRect[i].height > MIN_TARGET_HEIGHT &&
            boundRect[i].width <= MAX_TARGET_WIDTH && 
            boundRect[i].height <= MAX_TARGET_HEIGHT) {
                boundRect[i].y += y_offset;
                roiRect.push_back(boundRect[i]);
                if(++num_target >= MAX_NUM_TARGET)
                    break;
        }
    }    
}

/*
*
*/

void sig_handler(int signo)
{
    if(signo == SIGINT) {
        printf("SIGINT\n");
        bShutdown = true;
    } else if(signo == SIGHUP) {
        printf("SIGHUP\n");
        OnPushButton(f3xBase);
    }
}

/*
*
*/

#define PID_FILE "/var/run/dragon-eye.pid"

int main(int argc, char**argv)
{
    if(signal(SIGINT, sig_handler) == SIG_ERR)
        printf("\ncan't catch SIGINT\n");

    if(signal(SIGHUP, sig_handler) == SIG_ERR)
        printf("\ncan't catch SIGHUP\n");

    ofstream pf(PID_FILE); 
    if(pf) {
        pf << getpid();
        pf.close();
    } else
        cout << "Error open " << PID_FILE << endl;

    cuda::printShortCudaDeviceInfo(cuda::getDevice());
    std::cout << cv::getBuildInformation() << std::endl;

    f3xBase.UpdateSystemConfig();
    if(f3xBase.IsBaseHwSwitch()) /* Overwrite config by HW switch */
        f3xBase.UpdateHwSwitch();
    f3xBase.SetupGPIO();
    f3xBase.OpenTty();
    f3xBase.OpenUdpSocket();

    camera.UpdateCameraConfig();
    if(!camera.UpdateExposure())
        return 1;

    int cx = CAMERA_WIDTH - 1;
    int cy = (CAMERA_HEIGHT / 2) - 1;

    Ptr<cuda::BackgroundSubtractorMOG2> bsModel = cuda::createBackgroundSubtractorMOG2(90, 16, false); /* background history count, varThreshold, shadow detection */
    Ptr<cuda::BackgroundSubtractorMOG2> bsModel2 = cuda::createBackgroundSubtractorMOG2(90, 32, false); /* background history count, varThreshold, shadow detection */
    Ptr<cuda::Filter> gaussianFilter = cuda::createGaussianFilter(CV_8UC1, CV_8UC1, Size(5, 5), 3.5);
    Ptr<cuda::Filter> gaussianFilter2 = cuda::createGaussianFilter(CV_8UC1, CV_8UC1, Size(3, 3), 5.0);

    Tracker tracker;
    Target *primaryTarget = 0;

    cout << endl;
    cout << "### Press button to start object tracking !!!" << endl;

    double fps = 0;

    //high_resolution_clock::time_point t1(high_resolution_clock::now());
    steady_clock::time_point t1(steady_clock::now());

    uint64_t loopCount = 0;

    while(1) {
        if(bShutdown)
            break;

        static unsigned int vPushButton = 1;
        unsigned int gv = f3xBase.PushButton();
        if(gv == 0 && vPushButton == 1) { /* Raising edge */
            if(loopCount >= 10) { /* Button debunce */
                OnPushButton(f3xBase);
                loopCount = 0;
            }
        }
        vPushButton = gv;

        if(f3xBase.IsBaseRemoteControl()) {
            uint8_t data[1];
            if(f3xBase.ReadTty(data, 1)) {
                if(f3xBase.BaseType() == BASE_A) {
                    if((data[0] & 0xc0) == 0x80) {
                        uint8_t v = data[0] & 0x3f;
                        if(v == 0x00) { /* BaseA Off - 10xx xxx0 */
                            if(bPause == false) {
                                OnPushButton(f3xBase);
                                loopCount = 0;
                            }
                        } else if(v == 0x01) { /* BaseA On - 10xx xxx1 */
                            if(bPause == true) {
                                OnPushButton(f3xBase);
                                loopCount = 0;
                            }
                        }
                    }
                } else if(f3xBase.BaseType() == BASE_B) {
                    if((data[0] & 0xc0) == 0xc0) {
                        uint8_t v = data[0] & 0x3f;
                        if(v == 0x00) { /* BaseB Off - 11xx xxx0 */
                            if(bPause == false) {
                                OnPushButton(f3xBase);
                                loopCount = 0;
                            }
                        } else if(v == 0x01) { /* BaseB On - 11xx xxx1 */
                            if(bPause == true) {
                                OnPushButton(f3xBase);
                                loopCount = 0;
                            }
                        }
                    }
                }           
            }
        }
        
        loopCount++; /* Increase loop count */

        if(bPause) {
            f3xBase.RedLed(off);
            f3xBase.Relay(off);
            f3xBase.GreenLed(on);
            if(f3xBase.IsVideoOutputFile())
                f3xBase.BlueLed(on);

            usleep(10000); /* Wait 10ms */
            continue;
        }

        if(loopCount % 2 == 0) {
            f3xBase.GreenLed(on); /* Flash during frames */
            if(f3xBase.IsVideoOutputFile())
                f3xBase.BlueLed(on);
        } else {
            f3xBase.GreenLed(off); /* Flash during frames */
            if(f3xBase.IsVideoOutputFile())
                f3xBase.BlueLed(off);
        }

        Mat capFrame;
        camera.Read(capFrame);

        Mat outFrame;
        if(f3xBase.IsVideoOutputResult()) {
            if(f3xBase.IsVideoOutput()) {
                capFrame.copyTo(outFrame);
                line(outFrame, Point(0, cy), Point(cx, cy), Scalar(0, 255, 0), 1);
            }
        }

        int erosion_size = 6;   
        Mat element = cv::getStructuringElement(cv::MORPH_RECT,
                          cv::Size(2 * erosion_size + 1, 2 * erosion_size + 1), 
                          cv::Point(-1, -1) ); /* Default anchor point */

        /* Gray color space for whole region */
        
        Mat grayFrame;
        cvtColor(capFrame, grayFrame, COLOR_BGR2GRAY);
        cuda::GpuMat gpuFrame;

        erode(grayFrame, grayFrame, element);
        gpuFrame.upload(grayFrame);	

        cuda::GpuMat gpuForegroundMask;
        bsModel->apply(gpuFrame, gpuForegroundMask, -1);

        Mat foregroundMask;
        gaussianFilter->apply(gpuForegroundMask, gpuForegroundMask);
        gpuForegroundMask.download(foregroundMask);
        erode(foregroundMask, foregroundMask, element);

        vector<Rect> roiRect;

        contour_moving_object(foregroundMask, roiRect);

        /* HSV color space Hue channel for bottom 1/3 region */

        Mat hsvFrame, roiFrame;
        int y_offset = capFrame.rows * 2 / 3;
        capFrame(Rect(0, y_offset, capFrame.cols, capFrame.rows - y_offset)).copyTo(roiFrame);
        cvtColor(roiFrame, hsvFrame, COLOR_BGR2HSV);
        Mat hsvCh[3];
        split(hsvFrame, hsvCh);

        erode(hsvCh[0], hsvCh[0], element);
        gpuFrame.upload(hsvCh[0]); 

        bsModel2->apply(gpuFrame, gpuForegroundMask, -1);

        gaussianFilter2->apply(gpuForegroundMask, gpuForegroundMask);
        gpuForegroundMask.download(foregroundMask);
        erode(foregroundMask, foregroundMask, element);

        contour_moving_object(foregroundMask, roiRect, y_offset);

        tracker.Update(roiRect);

        f3xBase.RedLed(off);
        f3xBase.Relay(off);

        primaryTarget = tracker.PrimaryTarget();
        if(primaryTarget) {
            if(f3xBase.IsVideoOutputResult()) {
                if(f3xBase.IsVideoOutput()) {
                    Rect r = primaryTarget->m_rects.back();
                    rectangle(outFrame, r.tl(), r.br(), Scalar( 255, 0, 0 ), 2, 8, 0);
                    if(primaryTarget->m_points.size() > 1) { /* Minimum 2 points ... */
                        for(int i=0;i<primaryTarget->m_points.size()-1;i++) {
                            line(outFrame, primaryTarget->m_points[i], primaryTarget->m_points[i+1], Scalar(0, 255, 255), 1);
                        }
                    }
                }
            }
            if(primaryTarget->CourseLength() > MIN_COURSE_LENGTH && 
                    primaryTarget->TrackedCount() > MIN_TARGET_TRACKED_COUNT) {
                if((primaryTarget->m_points[0].y > cy && primaryTarget->LatestPoint().y <= cy) ||
                    (primaryTarget->m_points[0].y < cy && primaryTarget->LatestPoint().y >= cy)) {

                    if(primaryTarget->TriggerCount() < MAX_NUM_TRIGGER) { /* Triggle 3 times maximum  */
                        if(f3xBase.IsVideoOutputResult()) {
                            if(f3xBase.IsVideoOutput())
                                line(outFrame, Point(0, cy), Point(cx, cy), Scalar(0, 0, 255), 3);
                        }
                        f3xBase.TriggerTty(primaryTarget->TriggerCount() == 0);
                        f3xBase.TriggerUdpSocket(primaryTarget->TriggerCount() == 0);
                        f3xBase.RedLed(on);
                        f3xBase.Relay(on);

                        primaryTarget->Trigger();
                    }
                }
            }
        }
/*
        Mat background;
        bsModel->getBackgroundImage(background);
        if (!background.empty())
            imshow("mean background image", background );
*/
        if(f3xBase.IsVideoOutput()) {
            if(f3xBase.IsVideoOutputResult()) {
                char str[32];
                snprintf(str, 32, "FPS : %.2lf", fps);
                int fontFace = FONT_HERSHEY_SIMPLEX;
                const double fontScale = 1;
                const int thicknessScale = 1;  
                Point textOrg(40, 40);
                putText(outFrame, string(str), textOrg, fontFace, fontScale, Scalar(0, 255, 0), thicknessScale, cv::LINE_8);
                videoOutputQueue.push(outFrame.clone());
            } else {
                videoOutputQueue.push(capFrame.clone());
            }
        }

//        high_resolution_clock::time_point t2(high_resolution_clock::now());
        steady_clock::time_point t2(steady_clock::now());
        double dt_us(static_cast<double>(duration_cast<microseconds>(t2 - t1).count()));
        //std::cout << "FPS : " << fixed  <<  setprecision(2) << (1000000.0 / dt_us) << std::endl;
        fps = (1000000.0 / dt_us);
        std::cout << "FPS : " << fixed  << setprecision(2) <<  fps << std::endl;

//        t1 = high_resolution_clock::now();
        t1 = steady_clock::now();
    }

    f3xBase.GreenLed(off); /* Flash during frames */
    f3xBase.BlueLed(off); /* Flash during file save */
    f3xBase.RedLed(off); /* While object detected */
    f3xBase.Relay(off);

    if(f3xBase.IsVideoOutput()) {
        if(bPause == false) {
            videoOutputQueue.cancel();
            voThread.join();
        }
    }

    camera.Close();
    f3xBase.CloseTty();
    f3xBase.CloseUdpSocket();

    cout << endl;
    cout << "Finished ..." << endl;

    return 0;     
}
