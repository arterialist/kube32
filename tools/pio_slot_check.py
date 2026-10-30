# PlatformIO post-build hook: fails the build when firmware.bin does not fit
# the smallest app slot of partitions.csv. A firmware rollout writes the image
# into the slot that is not running, so both slots must hold it.
Import("env")
import os


def app_slots(path):
    slots = []
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            cols = [c.strip() for c in line.split(",")]
            if len(cols) >= 5 and cols[1] in ("app", "0"):
                slots.append((cols[0], int(cols[4], 0)))
    return slots


def check_slot_size(target, source, env):
    image = target[0].get_abspath()
    table = os.path.join(env["PROJECT_DIR"], env.GetProjectOption("board_build.partitions", "partitions.csv"))
    slots = app_slots(table)
    if not slots:
        print("slot check: no app partition in %s" % table)
        env.Exit(1)
    name, size = min(slots, key=lambda s: s[1])
    used = os.path.getsize(image)
    print("slot check: %s is %d bytes, %d bytes (%.1f%%) of app slot %s" % (os.path.basename(image), used, size, 100.0 * used / size, name))
    if used > size:
        print("slot check: the image is %d bytes larger than app slot %s" % (used - size, name))
        env.Exit(1)


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", check_slot_size)
