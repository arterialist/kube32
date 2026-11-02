"""kube32.io/limits: the safety limits of a Device, enforced on the actions that the MCP server sends.

One rule a line; a reason in plain words may follow a colon. Lines that start
with # are comments.

    deny tare: the scale holds a reference weight
    led.r <= 64: the LED faces a bedroom
    set every 30s: the relay is rated for 100,000 switching cycles
    humidity_pct <= 80: the enclosure is rated to 80 %RH

    deny <action>                      refuse the action; deny * refuses every action
    <action>.<argument> <op> <value>   refuse the action unless the argument satisfies the comparison
    <action> every <duration>          at most one such action per duration (ms, s, m or h); * counts all
    <reading> <op> <value>             the reading's safe range: readings outside it are flagged

<op> is one of <, <=, >, >=, ==, != (= means ==). <value> is a number, true,
false, or a word. A rule on an argument applies to the actions that name
that argument: an action without the argument is refused.

The rules fail closed: a line that does not parse, or a rule that names an
action, argument or reading the driver does not have, makes every action
on the Device refused until the annotation is fixed.
"""
import operator
import re

ANNOTATION = "kube32.io/limits"
NOTES_ANNOTATION = "kube32.io/notes"

_OPS = {"<": operator.lt, "<=": operator.le, ">": operator.gt, ">=": operator.ge, "==": operator.eq, "=": operator.eq,
        "!=": operator.ne}
_NAME = r"[A-Za-z_][A-Za-z0-9_-]*"
_DENY_RE = re.compile(r"^deny\s+(?P<action>%s|\*)$" % _NAME)
_RATE_RE = re.compile(r"^(?P<action>%s|\*)\s+every\s+(?P<n>\d+(?:\.\d+)?)\s*(?P<unit>ms|s|m|h)$" % _NAME)
_CMP_RE = re.compile(r"^(?P<target>%s(?:\.%s)?)\s*(?P<op><=|>=|==|!=|<|>|=)\s*(?P<value>\S+)$" % (_NAME, _NAME))
_SECONDS = {"ms": 0.001, "s": 1, "m": 60, "h": 3600}


class Rule:
    def __init__(self, line, text, reason, kind, action=None, arg=None, reading=None, op=None, value=None, seconds=None):
        self.line = line
        self.text = text          # the rule as written, without the reason
        self.reason = reason
        self.kind = kind          # deny, argument, rate, reading
        self.action = action
        self.arg = arg
        self.reading = reading
        self.op = op
        self.value = value
        self.seconds = seconds

    def __str__(self):
        return "%s: %s" % (self.text, self.reason) if self.reason else self.text

    def refusal(self, why):
        return "refused by the limit '%s' (kube32.io/limits, line %d): %s%s" % (
            self.text, self.line, why, "; the limit says: " + self.reason if self.reason else "")


class Limits:
    def __init__(self, rules=(), errors=()):
        self.rules = list(rules)
        self.errors = list(errors)   # "line <n>: ..." for lines that do not parse or rules that name nothing


def _value(text):
    if text in ("true", "false"):
        return text == "true"
    try:
        return int(text)
    except ValueError:
        pass
    try:
        return float(text)
    except ValueError:
        return text.strip('"')


def parse(text):
    """The rules of an annotation value, with an error for each line that does not parse."""
    rules, errors = [], []
    for n, raw in enumerate((text or "").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        rule_text, _, reason = line.partition(":")
        rule_text, reason = rule_text.strip(), reason.strip()
        m = _DENY_RE.match(rule_text)
        if m:
            rules.append(Rule(n, rule_text, reason, "deny", action=m.group("action")))
            continue
        m = _RATE_RE.match(rule_text)
        if m:
            seconds = float(m.group("n")) * _SECONDS[m.group("unit")]
            rules.append(Rule(n, rule_text, reason, "rate", action=m.group("action"), seconds=seconds))
            continue
        m = _CMP_RE.match(rule_text)
        if m:
            value = _value(m.group("value"))
            op = "==" if m.group("op") == "=" else m.group("op")
            if not isinstance(value, (int, float)) or isinstance(value, bool):
                if op not in ("==", "!="):
                    errors.append("line %d: '%s': %s compares numbers only" % (n, rule_text, op))
                    continue
            target = m.group("target")
            if "." in target:
                action, arg = target.split(".", 1)
                rules.append(Rule(n, rule_text, reason, "argument", action=action, arg=arg, op=op, value=value))
            else:
                rules.append(Rule(n, rule_text, reason, "reading", reading=target, op=op, value=value))
            continue
        errors.append("line %d: '%s' is not a rule (deny <action>, <action>.<argument> <op> <value>, "
                      "<action> every <duration>, or <reading> <op> <value>)" % (n, rule_text))
    return Limits(rules, errors)


def check_names(limits, readings, actions):
    """Adds an error for each rule that names a reading, action or argument the driver does not have.
    readings: {key: unit}; actions: {name: {argument: spec}}."""
    for r in limits.rules:
        if r.kind in ("deny", "rate") and r.action != "*" and r.action not in actions:
            limits.errors.append("line %d: '%s': the driver has no action %s (it has: %s)"
                                 % (r.line, r.text, r.action, ", ".join(sorted(actions)) or "none"))
        elif r.kind == "argument" and r.arg not in actions.get(r.action, {}):
            what = ("action %s has no argument %s" % (r.action, r.arg)) if r.action in actions else "the driver has no action %s" % r.action
            limits.errors.append("line %d: '%s': %s" % (r.line, r.text, what))
        elif r.kind == "reading" and r.reading not in readings:
            limits.errors.append("line %d: '%s': the driver has no reading %s (it reads: %s)"
                                 % (r.line, r.text, r.reading, ", ".join(sorted(readings)) or "nothing"))
    return limits


def _holds(op, have, want):
    """Whether `have <op> want` holds; a value of another type than the limit's never does."""
    if isinstance(want, bool) or isinstance(have, bool):
        same_type = isinstance(want, bool) and isinstance(have, bool)
    elif isinstance(want, (int, float)):
        same_type = isinstance(have, (int, float))
    else:
        same_type = isinstance(have, str)
    return same_type and _OPS[op](have, want)


def check_action(limits, action, args, last_action_at, now):
    """Why the limits refuse `action` with `args`, or None. last_action_at: {action: time of the last one
    sent} for this Device."""
    if limits.errors:
        return "the Device's kube32.io/limits do not parse, so every action is refused until they are fixed:\n  " + \
               "\n  ".join(limits.errors)
    for r in limits.rules:
        if r.kind == "deny" and r.action in (action, "*"):
            return r.refusal("the action %s is not allowed" % action)
        if r.kind == "argument" and r.action == action:
            if r.arg not in args:
                return r.refusal("the action must give %s" % r.arg)
            if not _holds(r.op, args[r.arg], r.value):
                return r.refusal("%s is %s" % (r.arg, _show(args[r.arg])))
        if r.kind == "rate" and r.action in (action, "*"):
            times = [t for a, t in last_action_at.items() if r.action == "*" or a == action]
            if times and now - max(times) < r.seconds:
                return r.refusal("the last one was %.1f s ago; wait %.1f s" % (now - max(times), r.seconds - (now - max(times))))
    return None


def check_readings(limits, readings):
    """A line for each reading outside its declared range."""
    out = []
    for r in limits.rules:
        if r.kind == "reading" and r.reading in readings and readings[r.reading] is not None:
            if not _holds(r.op, readings[r.reading], r.value):
                out.append("%s is %s, outside the limit '%s'%s" % (r.reading, _show(readings[r.reading]), r.text,
                                                                   " (" + r.reason + ")" if r.reason else ""))
    return out


def _show(v):
    return "true" if v is True else "false" if v is False else str(v)
