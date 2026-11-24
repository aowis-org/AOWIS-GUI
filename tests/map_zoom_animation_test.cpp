#include "map/core/map_frame_pacer.h"
#include "map/core/map_zoom_animation.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

namespace
{
bool require(bool valid, const char *reason)
{
    if (!valid)
        std::fprintf(stderr, "map_zoom_animation: %s\n", reason);
    return valid;
}

struct Sample
{
    double time = 0.0;
    double zoom = 0.0;
    double velocity = 0.0;
};

// Drives one MapZoomMotion like MapWidget does: input events are applied at
// the start of the frame in which they occur, then the frame is advanced.
// press_time/release_time < 0 disables the corresponding event.
std::vector<Sample> simulateHold(double press_time, double release_time,
                                 double duration,
                                 const std::function<double(int)> &frame_seconds)
{
    MapZoomMotion motion;
    motion.setLimits(1.0, 21.0);
    double zoom = 12.0;
    double time = 0.0;
    bool pressed = false;
    bool released = false;
    std::vector<Sample> samples;
    int frame = 0;
    while (time < duration - 1e-12)
    {
        if (!pressed && press_time >= 0.0 && time >= press_time - 1e-12)
        {
            motion.setHold(zoom, 1, 1.5, MapZoomAnimation::KeyboardTapOctaves);
            pressed = true;
        }
        if (pressed && !released && release_time >= 0.0 && time >= release_time - 1e-12)
        {
            motion.setHold(zoom, 0, 1.5, MapZoomAnimation::KeyboardTapOctaves);
            released = true;
        }
        const double step = std::min(frame_seconds(frame), duration - time);
        zoom = motion.advance(zoom, step);
        time += step;
        samples.push_back({time, zoom, motion.velocity()});
        ++frame;
    }
    return samples;
}

double valueAt(const std::vector<Sample> &samples, double time)
{
    for (const Sample &sample : samples)
    {
        if (std::abs(sample.time - time) < 1e-9)
            return sample.zoom;
    }
    return NAN;
}
}

int main()
{
    const std::function<double(int)> at60 = [](int) { return 1.0 / 60.0; };
    const std::function<double(int)> at144 = [](int) { return 1.0 / 144.0; };
    // Deterministic jitter whose frames still land on the 1/60 s grid every
    // 4 frames: 1/60 split unevenly as (0.3, 0.7) and (0.55, 0.45) pairs.
    const std::function<double(int)> jittered = [](int frame)
    {
        const double pattern[4] = {0.3, 0.7, 0.55, 0.45};
        return pattern[frame % 4] / 30.0;
    };

    // 1. Frame-rate independence (exact integration): hold 1 s, release, settle.
    {
        const std::vector<Sample> a = simulateHold(0.0, 1.0, 2.0, at60);
        const std::vector<Sample> b = simulateHold(0.0, 1.0, 2.0, at144);
        const std::vector<Sample> c = simulateHold(0.0, 1.0, 2.0, jittered);
        const double times[3] = {0.5, 1.0, 2.0};
        for (double time : times)
        {
            if (!require(std::abs(valueAt(a, time) - valueAt(b, time)) < 1e-9
                         && std::abs(valueAt(a, time) - valueAt(c, time)) < 1e-9,
                         "zoom trajectory depends on frame rate or frame jitter"))
                return 1;
        }
    }

    // 2. Hold: velocity rises from zero monotonically to the hold rate with no
    //    lurch (the former path started at 3.4 oct/s), then release decelerates
    //    without a velocity jump and without reversing.
    {
        const std::vector<Sample> s = simulateHold(0.0, 1.0, 2.0, at60);
        double previous_velocity = 0.0;
        double maximum_velocity_change = 0.0;
        for (const Sample &sample : s)
        {
            if (sample.time <= 1.0 + 1e-9)
            {
                if (!require(sample.velocity >= previous_velocity - 1e-12
                             && sample.velocity <= 1.5 + 1e-9,
                             "held zoom velocity is not a monotone ramp to the hold rate"))
                    return 1;
            }
            maximum_velocity_change = std::max(
                maximum_velocity_change, std::abs(sample.velocity - previous_velocity));
            if (!require(sample.velocity >= -1e-12, "zoom reversed after key release"))
                return 1;
            previous_velocity = sample.velocity;
        }
        // Largest per-frame velocity change at 60 Hz stays below 0.4 oct/s
        // (the former release alone was 1.5 oct/s in a single frame).
        if (!require(maximum_velocity_change < 0.4, "per-frame zoom velocity jump"))
            return 1;
        const double at_release = valueAt(s, 1.0);
        const double final_zoom = s.back().zoom;
        const double tail = final_zoom - at_release;
        if (!require(tail > 0.0 && tail <= 2.0 * 1.5 / MapZoomAnimation::SpringAngularFrequency + 1e-6,
                     "release tail exceeds the documented spring lag"))
            return 1;
        // Steady state: after the spring has caught up, hold speed is exact.
        const double v1 = (valueAt(s, 1.0) - valueAt(s, 0.5)) / 0.5;
        if (!require(std::abs(v1 - 1.5) < 1e-3, "held zoom does not reach the configured rate"))
            return 1;
        // Total motion = rate * hold time; nothing is lost or queued.
        if (!require(std::abs(final_zoom - (12.0 + 1.5)) < 1e-4,
                     "held zoom total does not equal rate times hold time"))
            return 1;
    }

    // 3. Quick tap completes to exactly one tap step and never overshoots.
    {
        const std::vector<Sample> s = simulateHold(0.0, 1.0 / 60.0, 1.5, at60);
        double previous = 12.0;
        for (const Sample &sample : s)
        {
            if (!require(sample.zoom >= previous - 1e-12
                         && sample.zoom <= 12.0 + MapZoomAnimation::KeyboardTapOctaves + 1e-12,
                         "keyboard tap overshot or reversed"))
                return 1;
            previous = sample.zoom;
        }
        if (!require(std::abs(s.back().zoom - (12.0 + MapZoomAnimation::KeyboardTapOctaves)) < 1e-9,
                     "keyboard tap did not complete to the tap step"))
            return 1;
        // First visible frame moves less than the old easing (0.056 octave).
        if (!require(s.front().zoom - 12.0 < 0.03, "keyboard tap starts with a jump"))
            return 1;
    }

    // 4. Successive wheel impulses accumulate; a reversed impulse starts from
    //    the visible camera, not from the stale target.
    {
        MapZoomMotion motion;
        motion.setLimits(1.0, 21.0);
        motion.addImpulse(12.0, 1.0);
        motion.addImpulse(12.0, 1.0);
        if (!require(std::abs(motion.target() - 14.0) < 1e-12, "wheel impulses did not accumulate"))
            return 1;
        double zoom = 12.0;
        for (int frame = 0; frame < 6; ++frame)
            zoom = motion.advance(zoom, 1.0 / 60.0);
        motion.addImpulse(zoom, -0.5);
        if (!require(std::abs(motion.target() - (zoom - 0.5)) < 1e-12,
                     "reversed wheel zoom reused a stale in-flight target"))
            return 1;
    }

    // 5. Limits: never exceeds, settles exactly at the bound and stops.
    {
        MapZoomMotion motion;
        motion.setLimits(1.0, 21.0);
        double zoom = 20.8;
        motion.setHold(zoom, 1, 1.5, MapZoomAnimation::KeyboardTapOctaves);
        for (int frame = 0; frame < 120; ++frame)
        {
            zoom = motion.advance(zoom, 1.0 / 60.0);
            if (!require(zoom <= 21.0, "zoom exceeded the maximum"))
                return 1;
        }
        motion.setHold(zoom, 0, 1.5, MapZoomAnimation::KeyboardTapOctaves);
        for (int frame = 0; frame < 120; ++frame)
            zoom = motion.advance(zoom, 1.0 / 60.0);
        if (!require(zoom == 21.0 && !motion.active(), "zoom did not settle at the maximum"))
            return 1;
    }

    // 6. Settling: an impulse ends with exact target and an inactive motion.
    {
        MapZoomMotion motion;
        motion.setLimits(1.0, 21.0);
        double zoom = 15.49;
        motion.addImpulse(zoom, 0.02);
        bool crossed_tile_boundary = false;
        int frames = 0;
        while (motion.active() && frames < 600)
        {
            const double next = motion.advance(zoom, 1.0 / 60.0);
            if (!require(next >= zoom && next <= 15.51, "zoom snapped at imagery boundary"))
                return 1;
            crossed_tile_boundary = crossed_tile_boundary || (zoom < 15.5 && next >= 15.5);
            zoom = next;
            ++frames;
        }
        if (!require(crossed_tile_boundary && zoom == 15.51 && frames < 60,
                     "continuous zoom did not settle across a tile boundary"))
            return 1;
    }

    // 7. External camera changes cancel the motion instead of fighting them.
    {
        MapZoomMotion motion;
        motion.setLimits(1.0, 21.0);
        double zoom = 10.0;
        motion.addImpulse(zoom, 2.0);
        zoom = motion.advance(zoom, 1.0 / 60.0);
        zoom = 7.0; // e.g. footer zoom field
        zoom = motion.advance(zoom, 1.0 / 60.0);
        if (!require(zoom == 7.0 && !motion.active(), "external zoom change was overridden"))
            return 1;
    }

    // 8. Invalid time steps never move the camera.
    {
        MapZoomMotion motion;
        motion.setLimits(1.0, 21.0);
        motion.addImpulse(12.0, 1.0);
        if (!require(motion.advance(12.0, 0.0) == 12.0 && motion.advance(12.0, -1.0) == 12.0
                     && motion.advance(12.0, NAN) == 12.0,
                     "zero, negative or NaN elapsed time changed the camera"))
            return 1;
    }

    // 9. Frame pacer: jittered vsync-locked intervals become exact periods and
    //    camera time never drifts more than half a period from wall time.
    {
        MapFramePacer pacer;
        const double period = 1.0 / 60.0;
        double wall = 0.0;
        double camera = 0.0;
        const double jitter[5] = {0.004, -0.003, 0.0025, -0.0035, 0.0};
        double vsync = 0.0;
        double previous_vsync = 0.0;
        for (int frame = 1; frame <= 600; ++frame)
        {
            // Callback time = presentation time + dispatch jitter.
            vsync = double(frame) * period;
            const double callback = vsync + jitter[frame % 5];
            const double previous_callback = previous_vsync + jitter[(frame - 1) % 5];
            const double measured = callback - previous_callback;
            const double presented = pacer.advance(measured, period);
            if (!require(std::abs(presented - period) < 1e-12,
                         "vsync-locked frame was not paced to exactly one period"))
                return 1;
            wall += measured;
            camera += presented;
            if (!require(std::abs(wall - camera) <= period * 0.5 + 1e-12,
                         "paced camera time drifted from wall time"))
                return 1;
            previous_vsync = vsync;
        }

        // Unknown refresh (WASM): the estimate converges, skipped frames are 2 periods.
        MapFramePacer estimating;
        double paced_total = 0.0;
        double measured_total = 0.0;
        for (int frame = 0; frame < 300; ++frame)
        {
            const double measured = (frame % 37 == 36) ? 2.0 / 144.0
                : 1.0 / 144.0 + ((frame % 2) ? 0.0004 : -0.0004);
            paced_total += estimating.advance(measured, 0.0);
            measured_total += measured;
        }
        if (!require(std::abs(estimating.estimatedPeriodSeconds() - 1.0 / 144.0) < 1e-4
                     && std::abs(paced_total - measured_total) <= 1.0 / 144.0,
                     "frame period estimate did not converge"))
            return 1;
    }

    std::puts("Spring zoom motion and frame pacing passed");
    return 0;
}
