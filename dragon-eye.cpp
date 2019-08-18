// Standard include files
#include <opencv2/opencv.hpp>
#include <opencv2/videoio.hpp>

#include <opencv2/cudacodec.hpp>
#include <opencv2/cudabgsegm.hpp>
#include <opencv2/cudaobjdetect.hpp>

#include <errno.h>
#include <fcntl.h> 
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <stdio.h>
#include <signal.h>
#include <unistd.h>

using namespace cv;
using namespace std;

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>

extern "C" {
#include "jetsonGPIO/jetsonGPIO.h"
}

using std::chrono::high_resolution_clock;
using std::chrono::duration_cast;
using std::chrono::microseconds;

#define CAMERA_1080P

#ifdef CAMERA_1080P
    #define CAMERA_WIDTH 1920
    #define CAMERA_HEIGHT 1080
    #define CAMERA_FPS 30
    #define MIN_TARGET_WIDTH 16
    #define MIN_TARGET_HEIGHT 16
    #define MAX_TARGET_WIDTH 480
    #define MAX_TARGET_HEIGHT 480
#else
    #define CAMERA_WIDTH 1280
    #define CAMERA_HEIGHT 720
    #define CAMERA_FPS 60
    #define MIN_TARGET_WIDTH 12
    #define MIN_TARGET_HEIGHT 12
    #define MAX_TARGET_WIDTH 320
    #define MAX_TARGET_HEIGHT 320
#endif

#define MAX_NUM_TARGET 3

#define VIDEO_OUTPUT_DIR "/home/gigijoe/Videos"
#define VIDEO_OUTPUT_FILE_NAME "longan"

//#define VIDEO_OUTPUT_RESULT_FRAME
#define VIDEO_OUTPUT_ORIGINAL_FRAME

#define MAX_NUM_TRIGGER 6
#define MIN_NUM_TRIGGER 3

typedef enum { BASE_A, BASE_B, BASE_TIMER, BASE_ANEMOMETER } BaseType;

static const BaseType baseType = BASE_B;

static bool bVideoOutputScreen = false;
static bool bVideoOutputFile = false;
static bool bShutdown = false;
static bool bPause = true;

void sig_handler(int signo)
{
    if(signo == SIGINT) {
        printf("SIGINT\n");
        bShutdown = true;
    }
}

/*
*
*/

static int set_interface_attribs (int fd, int speed, int parity)
{
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
    tty.c_cc[VTIME] = 5;            // 0.5 seconds read timeout

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

static void set_blocking(int fd, int should_block)
{
    struct termios tty;
    memset (&tty, 0, sizeof tty);
    if (tcgetattr (fd, &tty) != 0)
    {
            printf ("error %d from tggetattr\n", errno);
            return;
    }

    tty.c_cc[VMIN]  = should_block ? 1 : 0;
    tty.c_cc[VTIME] = 5;            // 0.5 seconds read timeout

    if (tcsetattr (fd, TCSANOW, &tty) != 0)
            printf ("error %d setting term attributes\n", errno);
}

static void base_trigger(int fd, bool newTrigger) 
{
    static uint8_t serNo = 0x3f;
    uint8_t data[1];

    if(!fd)
        return;

    if(newTrigger) { /* It's NEW trigger */
        if(++serNo > 0x3f)
            serNo = 0;
    }

    if(baseType == BASE_A) {
        data[0] = (serNo & 0x3f);
        printf("BASE_A[%d]\r\n", serNo);
        write(fd, data, 1);
    } else if(baseType == BASE_B) {
        data[0] = (serNo & 0x3f) | 0x40;
        printf("BASE_B[%d]\r\n", serNo);
        write(fd, data, 1);
    }
}

/*
*
*/

static inline bool ContoursSort(vector<cv::Point> contour1, vector<cv::Point> contour2)  
{  
    //return (contour1.size() > contour2.size()); /* Outline length */
    return (cv::contourArea(contour1) > cv::contourArea(contour2)); /* Area */
}  

inline void writeText( Mat & mat, const string text )
{
    int fontFace = FONT_HERSHEY_SIMPLEX;
    double fontScale = 1;
    int thickness = 1;  
    Point textOrg(40, 40);
    putText( mat, text, textOrg, fontFace, fontScale, Scalar(0, 255, 0), thickness, cv::LINE_8 );
}

class Target
{
protected:
    double m_arcLength;
    unsigned long m_frameTick;
    uint8_t m_triggerCount;

public:
    Target(Rect & roi, unsigned long frameTick) : m_arcLength(0), m_frameTick(frameTick), m_triggerCount(0) {
        m_rects.push_back(roi);
        m_points.push_back(roi.tl());
        m_frameTick = frameTick;
    }

    vector< Rect > m_rects;
    vector< Point > m_points;
    Point m_velocity;

    void Update(Rect & roi, unsigned long frameTick) {
        if(m_rects.size() > 0)
            m_arcLength += norm(roi.tl() - m_rects.back().tl());

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
    }

    void Reset() {
        Rect r = m_rects.back();
        Point p = r.tl();
        m_rects.clear();
        m_points.clear();
        m_rects.push_back(r);
        m_points.push_back(p);
        m_triggerCount = 0;
        //m_frameTick =
        m_arcLength = 0; /* TODO : Do clear this ? */
    }

    inline double ArcLength() { return m_arcLength; }
    inline unsigned long FrameTick() { return m_frameTick; }
    inline Rect & LatestRect() { return m_rects.back(); }
    inline Point & LatestPoint() { return m_points.back(); }
    inline void Trigger() { m_triggerCount++; }
    inline uint8_t TriggerCount() { return m_triggerCount; }
    //inline Point & Velocity() { return m_velocity; }
};

static inline bool TargetSort(Target & a, Target & b)
{
    return a.LatestRect().area() > b.LatestRect().area();
}

#define MAX_NUM_FRAME_MISSING_TARGET 15

class Tracker
{
private:
    unsigned long m_frameTick;

public:
    Tracker() : m_frameTick(0) {}

    list< Target > m_targets;

    void Update(Mat & frame, vector< Rect > & roiRect) {
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
            }
            if(i == roiRect.size()) { /* Target missing ... */
                if(m_frameTick - t->FrameTick() > MAX_NUM_FRAME_MISSING_TARGET) { /* Target still missing for over X frames */
#ifdef DEBUG            
                    Point p = t->m_points.back();
                    printf("lost target : %d, %d\n", p.x, p.y);
#endif
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
                if((r & roiRect[i]).area() > 0) { /* Next step tracked ... */
                    t->Update(roiRect[i], m_frameTick);
                    break;
                }

                unsigned long f = m_frameTick - t->FrameTick();
                r.x += t->m_velocity.x * f;
                r.y += t->m_velocity.y * f;
                if((r & roiRect[i]).area() > 0) { /* Next step tracked with velocity ... */
                    t->Update(roiRect[i], m_frameTick);
                    break;
                }
            }
            if(t == m_targets.end()) { /* New target */
                for(t=m_targets.begin();t!=m_targets.end();t++) { /* Check if exist target */
                    Rect r = t->m_rects.back();
                    unsigned long f = m_frameTick - t->FrameTick();
                    r.x += t->m_velocity.x * f;
                    r.y += t->m_velocity.y * f;

                    if(cv::norm(r.tl()-roiRect[i].tl()) < 120) { /* Target tracked with Euclidean distance ... */
                        t->Update(roiRect[i], m_frameTick);
                        break;
                    }
                }
                if(t == m_targets.end()) { /* New target */
                    m_targets.push_back(Target(roiRect[i], m_frameTick));
#ifdef DEBUG            
                    printf("new target : %d, %d\n", roiRect[i].tl().x, roiRect[i].tl().y);
#endif
                }
            }
        }
        m_frameTick++;
    }

    Target *PrimaryTarget() {
        if(m_targets.size() == 0)
            return 0;

        m_targets.sort(TargetSort);
        return &m_targets.front();
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
    uint32_t delayCount = 0;
    while(queue_.size() >= 30) { /* Prevent memory overflow ... */
        usleep(10000); /* Wait for 10 ms */
        if(++delayCount > 3)
            return; /* Drop frame */
    }

    std::unique_lock<std::mutex> mlock(mutex_);
    queue_.push(image);
    cond_.notify_one();
}

Mat FrameQueue::pop()
{
    std::unique_lock<std::mutex> mlock(mutex_);

    while (queue_.empty()) {
        if (cancelled_) {
            throw cancelled();
        }
        cond_.wait(mlock);
        if (cancelled_) {
            throw cancelled();
        }
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

FrameQueue videoWriterQueue;

void VideoWriterThread(int width, int height)
{    
    Size videoSize = Size((int)width,(int)height);
    char gstStr[320];

    VideoWriter outFile;
    VideoWriter outScreen;
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

    if(bVideoOutputFile) {
    /* Countclockwise rote 90 degree - nvvidconv flip-method=1 */
        snprintf(gstStr, 320, "appsrc ! video/x-raw, format=(string)BGR ! \
                   videoconvert ! video/x-raw, format=(string)I420, framerate=(fraction)%d/1 ! \
                   nvvidconv flip-method=1 ! omxh265enc preset-level=3 ! matroskamux ! filesink location=%s/%s%c%03d.mkv ", 
            30, VIDEO_OUTPUT_DIR, VIDEO_OUTPUT_FILE_NAME, (baseType == BASE_A) ? 'A' : 'B', videoOutoutIndex);
        outFile.open(gstStr, VideoWriter::fourcc('X', '2', '6', '4'), 30, videoSize);
        cout << "Vodeo output " << gstStr << endl;
    }

    if(bVideoOutputScreen) {
        snprintf(gstStr, 320, "appsrc ! video/x-raw, format=(string)BGR ! \
                       videoconvert ! video/x-raw, format=(string)I420, framerate=(fraction)%d/1 ! \
                       nvvidconv ! video/x-raw(memory:NVMM) ! \
                       nvoverlaysink sync=false -e ", 30);
        outScreen.open(gstStr, VideoWriter::fourcc('I', '4', '2', '0'), 30, Size(CAMERA_WIDTH, CAMERA_HEIGHT));
    }

    videoWriterQueue.reset();

    try {
        while(1) {
            Mat frame = videoWriterQueue.pop();
            if(bVideoOutputFile)
                outFile.write(frame);
            if(bVideoOutputScreen)
                outScreen.write(frame);
        }
    } catch (FrameQueue::cancelled & /*e*/) {
        // Nothing more to process, we're done
        std::cout << "FrameQueue " << " cancelled, worker finished." << std::endl;
        if(bVideoOutputFile)
            outFile.release();
        if(bVideoOutputScreen)
            outScreen.release();
    }    
}

int main(int argc, char**argv)
{
    double fps = 0;

    jetsonNanoGPIONumber redLED = gpio16; // Ouput
    jetsonNanoGPIONumber greenLED = gpio17; // Ouput
    jetsonNanoGPIONumber blueLED = gpio50; // Ouput
    jetsonNanoGPIONumber relayControl = gpio51; // Ouput

    jetsonNanoGPIONumber pushButton = gpio18; // Input
    jetsonNanoGPIONumber videoOutputScreenSwitch = gpio19; // Input
    jetsonNanoGPIONumber videoOutputFileSwitch = gpio20; // Input

    /* 
    * Do enable GPIO by /etc/profile.d/export-gpio.sh 
    */

    gpioExport(redLED);
    gpioExport(greenLED);
    gpioExport(blueLED);
    gpioExport(relayControl);

    gpioSetDirection(redLED, outputPin); /* Red LED on while detection */
    gpioSetValue(redLED, off);

    gpioSetDirection(greenLED, outputPin); /* Flash during frames */
    gpioSetValue(greenLED, on);

    gpioSetDirection(blueLED, outputPin); /* */
    gpioSetValue(blueLED, off);

    gpioSetDirection(relayControl, outputPin); /* */
    gpioSetValue(relayControl, off);

    gpioExport(pushButton);
    gpioExport(videoOutputScreenSwitch);
    gpioExport(videoOutputFileSwitch);

    gpioSetDirection(pushButton, inputPin); /* Pause / Restart */
    gpioSetDirection(videoOutputScreenSwitch, inputPin); /* Base A / B */
    gpioSetDirection(videoOutputFileSwitch, inputPin); /* Video output on / off */

    switch(baseType) {
        case BASE_A: printf("<<< BASE A\n");
            break;
        case BASE_B: printf("<<< BASE B\n");
            break;
        default: printf("<<< BASE Unknown\n");
            break;
    }

    const char *ttyName = "/dev/ttyTHS1";

    int ttyFd = open (ttyName, O_RDWR | O_NOCTTY | O_SYNC);
    if (ttyFd) {
        set_interface_attribs (ttyFd, B9600, 0);  // set speed to 9600 bps, 8n1 (no parity)
        set_blocking (ttyFd, 0);                // set no blocking
    } else
        printf ("error %d opening %s: %s\n", errno, ttyName, strerror (errno));

    if(signal(SIGINT, sig_handler) == SIG_ERR)
        printf("\ncan't catch SIGINT\n");

    Mat frame, capFrame;
    cuda::GpuMat gpuFrame;

    cuda::printShortCudaDeviceInfo(cuda::getDevice());
    std::cout << cv::getBuildInformation() << std::endl;

    static char gstStr[512];

/*
wbmode              : White balance affects the color temperature of the photo
                        flags: readable, writable
                        Enum "GstNvArgusCamWBMode" Default: 1, "auto"
                           (0): off              - GST_NVCAM_WB_MODE_OFF
                           (1): auto             - GST_NVCAM_WB_MODE_AUTO
                           (2): incandescent     - GST_NVCAM_WB_MODE_INCANDESCENT
                           (3): fluorescent      - GST_NVCAM_WB_MODE_FLUORESCENT
                           (4): warm-fluorescent - GST_NVCAM_WB_MODE_WARM_FLUORESCENT
                           (5): daylight         - GST_NVCAM_WB_MODE_DAYLIGHT
                           (6): cloudy-daylight  - GST_NVCAM_WB_MODE_CLOUDY_DAYLIGHT
                           (7): twilight         - GST_NVCAM_WB_MODE_TWILIGHT
                           (8): shade            - GST_NVCAM_WB_MODE_SHADE
                           (9): manual           - GST_NVCAM_WB_MODE_MANUAL
exposuretimerange   : Property to adjust exposure time range in nanoseconds
            Use string with values of Exposure Time Range (low, high)
            in that order, to set the property.
            eg: exposuretimerange="34000 358733000"
                        flags: readable, writable
                        String. Default: null
    # This sets exposure to 20 ms
    exposuretimerange="20000000 20000000"
tnr-mode            : property to select temporal noise reduction mode
                        flags: readable, writable
                        Enum "GstNvArgusCamTNRMode" Default: 1, "NoiseReduction_Fast"
                           (0): NoiseReduction_Off - GST_NVCAM_NR_OFF
                           (1): NoiseReduction_Fast - GST_NVCAM_NR_FAST
                           (2): NoiseReduction_HighQuality - GST_NVCAM_NR_HIGHQUALITY
ee-mode             : property to select edge enhnacement mode
                        flags: readable, writable
                        Enum "GstNvArgusCamEEMode" Default: 1, "EdgeEnhancement_Fast"
                           (0): EdgeEnhancement_Off - GST_NVCAM_EE_OFF
                           (1): EdgeEnhancement_Fast - GST_NVCAM_EE_FAST
                           (2): EdgeEnhancement_HighQuality - GST_NVCAM_EE_HIGHQUALITY
*/
    int index = 0;    
    if(argc > 1)
        index = atoi(argv[1]);
    /* export GST_DEBUG=2 to show debug message */
    snprintf(gstStr, 512, "nvarguscamerasrc wbmode=6 tnr-mode=2 ee-mode=2 exposuretimerange=\"20000000 20000000\" ! \
        video/x-raw(memory:NVMM), width=(int)%d, height=(int)%d, format=(string)NV12, framerate=(fraction)%d/1 ! \
        nvvidconv flip-method=2 ! video/x-raw, format=(string)BGRx ! videoconvert ! video/x-raw, format=(string)BGR ! appsink max-buffers=1 drop=true -e ", 
        CAMERA_WIDTH, CAMERA_HEIGHT, CAMERA_FPS);
    VideoCapture cap(gstStr, cv::CAP_GSTREAMER);

    cout << "Video input " << gstStr << endl;

    if(!cap.isOpened()) {
        cout << "Could not open video" << endl;
        return 1;
    }

    thread outThread;

    //Ptr<BackgroundSubtractor> bsModel = createBackgroundSubtractorKNN();
    //Ptr<BackgroundSubtractor> bsModel = createBackgroundSubtractorMOG2();
    Ptr<cuda::BackgroundSubtractorMOG2> bsModel = cuda::createBackgroundSubtractorMOG2(30, 16, false);

    bool doUpdateModel = true;
    bool doSmoothMask = true;

    Mat foregroundMask, background;
#ifdef VIDEO_OUTPUT_RESULT_FRAME
    Mat outFrame;
#endif
    cuda::GpuMat gpuForegroundMask;
    Ptr<cuda::Filter> gaussianFilter;
    Ptr<cuda::Filter> erodeFilter;
    Ptr<cuda::Filter> erodeFilter2;

    Tracker tracker;
    Target *primaryTarget = 0;

    int cx, cy;
    while(1) {
        if(cap.read(capFrame))
            break;
        if(bShutdown)
            return 0;
    }
    cx = capFrame.cols - 1;
    cy = (capFrame.rows / 2) - 1;

    high_resolution_clock::time_point t1(high_resolution_clock::now());

    uint64_t frameCount = 0;
    unsigned int vPushButton = 1; /* Initial high */

    while(cap.read(capFrame)) {
        unsigned int gv;
        gpioGetValue(pushButton, &gv);

        frameCount++;

        if(gv == 1 && vPushButton == 0) { /* Raising edge */
            if(frameCount < 30) { /* Button debunce */
                usleep(1000);
                continue;
            }
            bPause = !bPause;
            frameCount = 0;
        }
        vPushButton = gv;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ttyFd, &rfds);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 0;

        if(select(ttyFd+1, &rfds, NULL, NULL, &tv) > 0) {
            uint8_t data[1];
            int r = read(ttyFd, data, 1); /* Receive trigger from f3f timer */
            if(r == 1) {
                if(baseType == BASE_A) {
                    if((data[0] & 0xc0) == 0x80) {
                        uint8_t v = data[0] & 0x3f;
                        if(v == 0x00) { /* BaseA Off - 10xx xxx0 */
                            if(bPause == false) {
                                bPause = true;
                                frameCount = 0;
                            }
                        } else if(v == 0x01) { /* BaseA On - 10xx xxx1 */
                            if(bPause == true) {
                                bPause = false;
                                frameCount = 0;
                            }
                        }
                    }
                } else if(baseType == BASE_B) {
                    if((data[0] & 0xc0) == 0xc0) {
                        uint8_t v = data[0] & 0x3f;
                        if(v == 0x00) { /* BaseB Off - 11xx xxx0 */
                            if(bPause == false) {
                                bPause = true;
                                frameCount = 0;
                            }
                        } else if(v == 0x01) { /* BaseB On - 11xx xxx1 */
                            if(bPause == true) {
                                bPause = false;
                                frameCount = 0;
                            }
                        }
                    }
                }
            }
        }

        if(frameCount == 0) { /* suspend or resume */
            if(bPause) {
                cout << "Paused ..." << endl;
                if(bVideoOutputScreen || bVideoOutputFile) {
                    videoWriterQueue.cancel();
                    outThread.join();
                }
            } else {/* Restart, record new video */
                cout << "Restart ..." << endl;
                //unsigned int gv;
                gpioGetValue(videoOutputScreenSwitch, &gv);
                if(gv == 0) /* pull low */
                    bVideoOutputScreen = true;
                else                    
                    bVideoOutputScreen = false;

                printf("<<< Video output screen : %s\n", bVideoOutputScreen ? "Enable" : "Disable");

                gpioGetValue(videoOutputFileSwitch, &gv);
                if(gv == 0)
                    bVideoOutputFile = true;
                else
                    bVideoOutputFile = false;

                printf("<<< Video output file : %s\n", bVideoOutputFile ? "Enable" : "Disable");
                if(bVideoOutputScreen || bVideoOutputFile)
                    outThread = thread(&VideoWriterThread, capFrame.cols, capFrame.rows);
            }
        }

        if(bPause) {
            if(bShutdown)
                break;
            gpioSetValue(redLED, off);
            gpioSetValue(greenLED, on);
            usleep(10000); /* Wait 10ms */
            continue;
        }

        if(frameCount % 2 == 0)
            gpioSetValue(greenLED, on);
        else
            gpioSetValue(greenLED, off);

        cvtColor(capFrame, frame, COLOR_BGR2GRAY);
#ifdef VIDEO_OUTPUT_RESULT_FRAME
        if(bVideoOutputScreen || bVideoOutputFile) {
            capFrame.copyTo(outFrame);
            line(outFrame, Point(0, cy), Point(cx, cy), Scalar(0, 255, 0), 1);
        }
#endif //VIDEO_OUTPUT_RESULT_FRAME
        int erosion_size = 6;   
        Mat element = cv::getStructuringElement(cv::MORPH_RECT,
                          cv::Size(2 * erosion_size + 1, 2 * erosion_size + 1), 
                          cv::Point(erosion_size, erosion_size) );
#if 0 /* Very poor performance ... Running by CPU is 10 times quick */
        gpuFrame.upload(frame);
        if(erodeFilter.empty()) 
            erodeFilter = cuda::createMorphologyFilter(MORPH_ERODE, gpuFrame.type(), element);
        erodeFilter->apply(gpuFrame, gpuFrame);
#else
        erode(frame, frame, element);
        gpuFrame.upload(frame);	
#endif    
        bsModel->apply(gpuFrame, gpuForegroundMask, doUpdateModel ? -1 : 0);

        if(gaussianFilter.empty())
            gaussianFilter = cuda::createGaussianFilter(gpuForegroundMask.type(), gpuForegroundMask.type(), Size(5, 5), 3.5);

        if(doSmoothMask) {
            gaussianFilter->apply(gpuForegroundMask, gpuForegroundMask);
            /* Disable threadhold while low background noise */
            /* 10.0 may be good senstitive */
            //cuda::threshold(gpuForegroundMask, gpuForegroundMask, 10.0, 255.0, THRESH_BINARY);
            /* 40.0 with lower senstitive */
            //cuda::threshold(gpuForegroundMask, gpuForegroundMask, 40.0, 255.0, THRESH_BINARY);
            
			/* Erode & Dilate */
            int erosion_size = 6;   
            Mat element = cv::getStructuringElement(cv::MORPH_RECT,
                          cv::Size(2 * erosion_size + 1, 2 * erosion_size + 1), 
                          cv::Point(erosion_size, erosion_size) );
#if 0
        if(erodeFilter2.empty()) 
            erodeFilter2 = cuda::createMorphologyFilter(MORPH_ERODE, gpuForegroundMask.type(), element);
        erodeFilter2->apply(gpuForegroundMask, gpuForegroundMask);
        gpuForegroundMask.download(foregroundMask);
#else
            gpuForegroundMask.download(foregroundMask);
            erode(foregroundMask, foregroundMask, element);
#endif
        } else
            gpuForegroundMask.download(foregroundMask);

		vector< vector<Point> > contours;
    	vector< Vec4i > hierarchy;
    	findContours(foregroundMask, contours, hierarchy, RETR_TREE, CHAIN_APPROX_NONE);
        sort(contours.begin(), contours.end(), ContoursSort); /* Contours sort by area, controus[0] is largest */

        vector<Rect> boundRect( contours.size() );
        vector<Rect> roiRect;

        RNG rng(12345);

    	for(int i=0; i<contours.size(); i++) {
    		approxPolyDP( Mat(contours[i]), contours[i], 3, true );
       		boundRect[i] = boundingRect( Mat(contours[i]) );
       		//drawContours(contoursImg, contours, i, color, 2, 8, hierarchy);
    		if(boundRect[i].width > MIN_TARGET_WIDTH && 
                boundRect[i].height > MIN_TARGET_HEIGHT &&
    			boundRect[i].width <= MAX_TARGET_WIDTH && 
                boundRect[i].height <= MAX_TARGET_HEIGHT) {

                    roiRect.push_back(boundRect[i]);
#ifdef VIDEO_OUTPUT_RESULT_FRAME
                    if(bVideoOutputScreen || bVideoOutputFile) {
                        Scalar color = Scalar( rng.uniform(0, 255), rng.uniform(0,255), rng.uniform(0,255) );
                        rectangle( outFrame, boundRect[i].tl(), boundRect[i].br(), color, 2, 8, 0 );
                    }
#endif
    		}
            if(roiRect.size() >= MAX_NUM_TARGET) /* Deal top 5 only */
                break;
    	}

        tracker.Update(frame, roiRect);

        gpioSetValue(redLED, off);

        primaryTarget = tracker.PrimaryTarget();
        if(primaryTarget) {
#ifdef VIDEO_OUTPUT_RESULT_FRAME
            if(bVideoOutputScreen || bVideoOutputFile) {
                Rect r = primaryTarget->m_rects.back();
                rectangle( outFrame, r.tl(), r.br(), Scalar( 255, 0, 0 ), 2, 8, 0 );

                if(primaryTarget->m_points.size() > 1) { /* Minimum 2 points ... */
                    for(int i=0;i<primaryTarget->m_points.size()-1;i++) {
                        line(outFrame, primaryTarget->m_points[i], primaryTarget->m_points[i+1], Scalar(0, 255, 255), 1);
                    }
                }
            }
#endif
            if(primaryTarget->ArcLength() > 120) {
                if((primaryTarget->m_points[0].y > cy && primaryTarget->LatestPoint().y <= cy) ||
                    (primaryTarget->m_points[0].y < cy && primaryTarget->LatestPoint().y >= cy)) {
#ifdef VIDEO_OUTPUT_RESULT_FRAME
                    if(bVideoOutputScreen || bVideoOutputFile) {
                        line(outFrame, Point(0, cy), Point(cx, cy), Scalar(0, 0, 255), 3);
                    }
#endif //VIDEO_OUTPUT_RESULT_FRAME               
                    if(primaryTarget->TriggerCount() < MAX_NUM_TRIGGER) { /* Triggle 3 times maximum  */
                        if(primaryTarget->TriggerCount() >= MIN_NUM_TRIGGER) /* Ignore first trigger to filter out fake detection */
                            base_trigger(ttyFd, primaryTarget->TriggerCount() == MIN_NUM_TRIGGER ? true : false); 
                        primaryTarget->Trigger();
                    } else
                        primaryTarget->Reset();
                    gpioSetValue(redLED, on);
                }
            }
        }
/*
        bsModel->getBackgroundImage(background);
        if (!background.empty())
            imshow("mean background image", background );
*/
        if(bVideoOutputScreen || bVideoOutputFile) {
#ifdef VIDEO_OUTPUT_ORIGINAL_FRAME
            videoWriterQueue.push(capFrame.clone());
#else
            char str[32];
            snprintf(str, 32, "FPS : %.2lf", fps);
            writeText(outFrame, string(str));

            videoWriterQueue.push(outFrame.clone());
#endif
        }

        if(bShutdown)
            break;

        high_resolution_clock::time_point t2(high_resolution_clock::now());
        double dt_us(static_cast<double>(duration_cast<microseconds>(t2 - t1).count()));
        //std::cout << "FPS : " << fixed  <<  setprecision(2) << (1000000.0 / dt_us) << std::endl;
        fps = (1000000.0 / dt_us);
        std::cout << "FPS : " << fixed  << setprecision(2) <<  fps << std::endl;

        t1 = high_resolution_clock::now();
    }

    gpioSetValue(greenLED, off);
    gpioSetValue(redLED, off);

    if(bVideoOutputScreen || bVideoOutputFile) {
        if(bPause == false) {
            videoWriterQueue.cancel();
            outThread.join();
        }
    }

    if(ttyFd)
        close(ttyFd);

    std::cout << "Finished ..." << endl;

    return 0;     
}