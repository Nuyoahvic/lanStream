#ifdef PLATFORM_MACOS

#import "capture_cg.hpp"
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <CoreFoundation/CoreFoundation.h>
#import <objc/message.h>

#include <chrono>
#include <vector>
#include <memory>
#include <sstream>

// Helper: implement SCStreamOutput protocol to receive sample buffers via a block.
@interface StreamOutputHandler : NSObject <SCStreamOutput>
@property (nonatomic, copy) void (^captureHandler)(CMSampleBufferRef sampleBuffer, SCStreamOutputType type);
@end

@implementation StreamOutputHandler
- (void)stream:(SCStream *)stream
didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
        ofType:(SCStreamOutputType)type
{
    if (self.captureHandler) {
        self.captureHandler(sampleBuffer, type);
    }
}
@end

// Internal implementation struct, encapsulating an Objective-C object
struct CaptureCG::Impl {
    SCStream* stream = nil;
    SCStreamConfiguration* config = nil;
    SCDisplay* selectedDisplay = nil;
};

// Anonymous namespace to avoid symbol conflicts
namespace {
    // Find the monitor at the specified index
    SCDisplay* findDisplayByIndex(int index) {
        __block SCDisplay* targetDisplay = nil;
        __block int counter = 0;
        __block bool done = false;

        [SCShareableContent getShareableContentWithCompletionHandler:
         ^(SCShareableContent *content, NSError *error) {
            if (error) {
                NSLog(@"Failed to retrieve shared content: %@", error);
                done = true;
                return;
            }

            for (SCDisplay* display in content.displays) {
                if (counter == index) {
                    CFRetain((__bridge CFTypeRef)display);
                    targetDisplay = display;
                    break;
                }
                counter++;
            }
            done = true;
        }];

        NSDate* timeout = [NSDate dateWithTimeIntervalSinceNow:1.0];
        while (!done && [[NSDate date] compare:timeout] == NSOrderedAscending) {
            [[NSRunLoop currentRunLoop]
             runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
        }

        if (!targetDisplay && counter > 0) {
            done = false;
            [SCShareableContent getShareableContentWithCompletionHandler:
             ^(SCShareableContent *content, NSError *error) {
                if (!error && content.displays.count > 0) {
                    SCDisplay* first = content.displays.firstObject;
                    CFRetain((__bridge CFTypeRef)first);
                    targetDisplay = first;
                }
                done = true;
            }];

            timeout = [NSDate dateWithTimeIntervalSinceNow:1.0];
            while (!done && [[NSDate date] compare:timeout] == NSOrderedAscending) {
                [[NSRunLoop currentRunLoop]
                 runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
            }
        }

        return targetDisplay;
    }
}

CaptureCG::~CaptureCG() {
    stop();
}

bool CaptureCG::init(int display_index) {
    if (display_index < 0) {
        NSLog(@"Invalid monitor index: %d", display_index);
        return false;
    }

    display_index_ = display_index;

    // Here, display.width/height are used merely as placeholders for "logical dimensions"; the actual pixel values ​​are calculated in start() using contentRect.
    SCDisplay* display = findDisplayByIndex(display_index_);
    if (!display) {
        NSLog(@"Monitor not found index=%d", display_index_);
        return false;
    }

    NSUInteger dwidth = 0;
    NSUInteger dheight = 0;
    @try {
        dwidth = (NSUInteger)display.width;
        dheight = (NSUInteger)display.height;
    } @catch (NSException *ex) {
        NSLog(@"Error reading monitor size: %@", ex);
    }
    CFRelease((__bridge CFTypeRef)display);

    width_  = (int)dwidth;
    height_ = (int)dheight;

    if (width_ <= 0 || height_ <= 0) {
        NSLog(@"Invalid monitor size: %dx%d", width_, height_);
        return false;
    }

    NSLog(@"Initialize display: Index=%d, Size (point)=%dx%d", display_index_, width_, height_);
    return true;
}

bool CaptureCG::start(FrameCallback cb) {
    if (running_.load()) {
        NSLog(@"Capture already running");
        return false;
    }

    if (width_ <= 0 || height_ <= 0) {
        if (!init(0)) {
            NSLog(@"Failed to automatically initialize the display.");
            return false;
        }
    }

    cb_ = cb;
    running_.store(true);

    thread_ = std::thread([this] {
        @autoreleasepool {
            std::unique_ptr<Impl> impl(new Impl());

            // 1. Get the display
            impl->selectedDisplay = findDisplayByIndex(display_index_);
            if (!impl->selectedDisplay) {
                NSLog(@"Failed to retrieve the display within the thread.");
                running_.store(false);
                return;
            }

            // 2. Create stream configuration
            impl->config = [[SCStreamConfiguration alloc] init];
            impl->config.width  = (size_t)width_;
            impl->config.height = (size_t)height_;
            impl->config.scalesToFit = NO;
            impl->config.preservesAspectRatio = YES;
            impl->config.queueDepth = 2;          // Low latency [web:27]
            impl->config.showsCursor = YES;
            impl->config.pixelFormat = kCVPixelFormatType_32BGRA;
            impl->config.colorSpaceName = kCGColorSpaceSRGB;
            impl->config.minimumFrameInterval = CMTimeMake(1, 60);

            NSError* error = nil;
            Class streamClass = NSClassFromString(@"SCStream");
            Class filterClass = NSClassFromString(@"SCContentFilter");
            if (!streamClass || !filterClass) {
                NSLog(@"The current system does not support this. ScreenCaptureKit");
                running_.store(false);
                return;
            }

            // 3. Construct SCContentFilter (capture the entire display)
            __block NSArray<SCRunningApplication*>* allApps = nil;
            __block bool appsReady = false;
            [SCShareableContent getShareableContentWithCompletionHandler:
             ^(SCShareableContent* content, NSError* err) {
                if (!err) {
                    allApps = content.applications;
                } else {
                    NSLog(@"Failed to retrieve the application list: %@", err);
                }
                appsReady = true;
            }];
            NSDate* appTimeout = [NSDate dateWithTimeIntervalSinceNow:2.0];
            while (!appsReady && [[NSDate date] compare:appTimeout] == NSOrderedAscending) {
                [[NSRunLoop currentRunLoop]
                 runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
            }

            SEL selWithApps =
                NSSelectorFromString(@"initWithDisplay:includingApplications:exceptingWindows:");
            SEL selExcludeApps =
                NSSelectorFromString(@"initWithDisplay:excludingApplications:exceptingWindows:");
            SEL selExcludeWindows =
                NSSelectorFromString(@"initWithDisplay:excludingWindows:");

            id filterAlloc = ((id (*)(Class, SEL))objc_msgSend)(
                filterClass, sel_registerName("alloc"));
            id contentFilter = nil;

            if (allApps && [filterClass instancesRespondToSelector:selWithApps]) {
                contentFilter = ((id (*)(id, SEL, id, id, id))objc_msgSend)(
                    filterAlloc, selWithApps, impl->selectedDisplay, allApps, @[]);
            } else if ([filterClass instancesRespondToSelector:selExcludeApps]) {
                NSArray<SCRunningApplication*>* emptyApps = @[];
                NSArray<SCWindow*>* emptyWindows = @[];
                contentFilter = ((id (*)(id, SEL, id, id, id))objc_msgSend)(
                    filterAlloc, selExcludeApps, impl->selectedDisplay,
                    emptyApps, emptyWindows);
            } else if ([filterClass instancesRespondToSelector:selExcludeWindows]) {
                contentFilter = ((id (*)(id, SEL, id, id))objc_msgSend)(
                    filterAlloc, selExcludeWindows, impl->selectedDisplay, @[]);
            }

            if (!contentFilter) {
                NSLog(@"Unable to create SCContentFilter");
                running_.store(false);
                return;
            }

            // 4. Set the actual pixel resolution using contentRect × pointPixelScale [web:20][web:23]
            SCContentFilter *filter = (SCContentFilter *)contentFilter;
            CGFloat scale = filter.pointPixelScale;
            CGRect rect = filter.contentRect;
            size_t pixelWidth  = (size_t)(CGRectGetWidth(rect)  * scale);
            size_t pixelHeight = (size_t)(CGRectGetHeight(rect) * scale);

            impl->config.width  = pixelWidth;
            impl->config.height = pixelHeight;
            impl->config.scalesToFit = NO;
            impl->config.preservesAspectRatio = YES;

            NSLog(@"SCStream capture size: %zux%zu (scale=%.2f, rect=%.0fx%.0f)",
                  pixelWidth, pixelHeight,
                  scale,
                  CGRectGetWidth(rect), CGRectGetHeight(rect));

            if ([impl->config respondsToSelector:@selector(setCaptureResolution:)]) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability"
                impl->config.captureResolution = SCCaptureResolutionBest;
#pragma clang diagnostic pop
            }

            // 5. Create SCStream
            SEL selInitFilterNoError =
                NSSelectorFromString(@"initWithFilter:configuration:delegate:");
            id alloced = ((id (*)(Class, SEL))objc_msgSend)(
                streamClass, sel_registerName("alloc"));
            id streamObj = nil;

            if ([streamClass instancesRespondToSelector:selInitFilterNoError]) {
                streamObj = ((id (*)(id, SEL, id, id, id))objc_msgSend)(
                    alloced, selInitFilterNoError, contentFilter, impl->config, nil);
            } else {
                NSLog(@"SCStream initialization method is unavailable.");
            }

            impl->stream = (SCStream*)streamObj;
            if (!impl->stream) {
                NSLog(@"Failed to create SCStream.");
                running_.store(false);
                return;
            }

            // 6. Add video output
            StreamOutputHandler* output = [[StreamOutputHandler alloc] init];
            __block Impl* implPtr = impl.get();
            output.captureHandler = ^(CMSampleBufferRef sampleBuffer,
                                      SCStreamOutputType type) {
                if (!running_.load()) return;
                if (type != SCStreamOutputTypeScreen) return;

                CVImageBufferRef imageBuffer =
                    CMSampleBufferGetImageBuffer(sampleBuffer);
                if (!imageBuffer) return;

                OSType pixelFormat =
                    CVPixelBufferGetPixelFormatType(imageBuffer);
                if (pixelFormat != kCVPixelFormatType_32BGRA) {
                    NSLog(@"Unsupported pixel format: %u", (unsigned int)pixelFormat);
                    return;
                }

                CVPixelBufferLockBaseAddress(imageBuffer,
                                             kCVPixelBufferLock_ReadOnly);

                size_t width       = CVPixelBufferGetWidth(imageBuffer);
                size_t height      = CVPixelBufferGetHeight(imageBuffer);
                size_t bytesPerRow = CVPixelBufferGetBytesPerRow(imageBuffer);
                void* baseAddress  = CVPixelBufferGetBaseAddress(imageBuffer);

                if (baseAddress && width > 0 && height > 0 && bytesPerRow > 0) {
                    auto frame = std::make_shared<RawFrame>();
                    frame->timestamp_us = now_us();
                    frame->width        = (int)width;
                    frame->height       = (int)height;
                    frame->linesize     = (int)bytesPerRow;
                    frame->format       = PixelFormat::BGRA;

                    size_t dataSize = bytesPerRow * height;
                    frame->data.resize(dataSize);
                    memcpy(frame->data.data(), baseAddress, dataSize);

                    if (cb_ && !frame->data.empty()) {
                        cb_(std::move(frame));   // Place encoding and sending in an external thread.
                    }
                }

                CVPixelBufferUnlockBaseAddress(imageBuffer,
                                               kCVPixelBufferLock_ReadOnly);
            };

            error = nil;
            dispatch_queue_t queue =
                dispatch_queue_create("capture.queue", DISPATCH_QUEUE_SERIAL);
            BOOL success = [impl->stream addStreamOutput:output
                                                    type:SCStreamOutputTypeScreen
                                    sampleHandlerQueue:queue
                                                 error:&error];
            if (!success) {
                NSLog(@"Failed to add stream output: %@", error);
                running_.store(false);
                return;
            }

            // 7. Start capturing
            [impl->stream startCaptureWithCompletionHandler:
            ^(NSError* _Nullable err) {
                if (err) {
                    NSLog(@"Failed to start capture: %@", err);
                    running_.store(false);
                } else {
                    NSLog(@"Screen capture started, stream=%p, pixel dimensions: %zux%zu",
                            (__bridge void*)implPtr->stream,
                            implPtr->config.width, implPtr->config.height);

                }
            }];

            // 8. Run the message loop
            NSRunLoop* runLoop = [NSRunLoop currentRunLoop];
            while (running_.load()) {
                @autoreleasepool {
                    [runLoop runUntilDate:
                     [NSDate dateWithTimeIntervalSinceNow:0.016]];
                }
            }

            // 9. Cleanup
            NSLog(@"Stop capturing...");
            if (impl->stream) {
                [impl->stream stopCaptureWithCompletionHandler:
                 ^(NSError* _Nullable err) {
                    if (err) {
                        NSLog(@"Error while stopping capture: %@", err);
                    } else {
                        NSLog(@"Capture stopped");
                    }
                }];
                impl->stream = nil;
            }
            if (impl->selectedDisplay) {
                CFRelease((__bridge CFTypeRef)impl->selectedDisplay);
                impl->selectedDisplay = nil;
            }
            NSLog(@"Capture thread exit");
        }
    });

    for (int i = 0; i < 50 && !running_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!running_.load()) {
        NSLog(@"Failed to start the capture thread.");
        if (thread_.joinable()) {
            thread_.join();
        }
        return false;
    }

    return true;
}

void CaptureCG::stop() {
    if (!running_.load()) return;

    NSLog(@"Stopping capture...");
    running_.store(false);

    if (thread_.joinable()) {
        thread_.join();
    }

    cb_ = nullptr;
    NSLog(@"Capture has stopped completely.");
}

#endif // PLATFORM_MACOS
