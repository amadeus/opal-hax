import time
import GPIO

def w(s):
    node.warn('gpiox: ' + s)

w('dir(GPIO)=' + str([n for n in dir(GPIO) if not n.startswith('__')]))
for name in ['read', 'write', 'setup', 'release', 'setInterrupt', 'hasInterruptEvent', 'waitInterruptEvent']:
    f = getattr(GPIO, name, None)
    w('%s: %s' % (name, (str(f.__doc__) if f else 'MISSING').replace('\n', ' ')[:400]))

def tryit(label, fn):
    try:
        w('%s -> %r' % (label, fn()))
    except Exception as e:
        w('%s !! %s' % (label, e))

tryit('read(58) before setup', lambda: GPIO.read(58))
tryit('setup(58, OUT, PULL_NONE, exclusive=False)', lambda: GPIO.setup(58, GPIO.OUT, GPIO.PULL_NONE, False))
tryit('read(58) after setup', lambda: GPIO.read(58))

last = None
t0 = time.monotonic()
while True:
    try:
        v = GPIO.read(58)
    except Exception as e:
        v = 'ERR ' + str(e)
    if v != last or time.monotonic() - t0 > 5:
        w('poll read(58)=%r' % (v,))
        last = v
        t0 = time.monotonic()
    time.sleep(0.2)
