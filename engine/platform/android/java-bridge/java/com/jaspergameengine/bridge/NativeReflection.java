package com.jaspergameengine.bridge;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.lang.reflect.Array;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.math.BigInteger;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.Comparator;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

// How Lua calls reach Java (pure Java: unit-tested on the JVM, NativeReflectionTest): the JSON
// text of the arguments is parsed, a public static method is picked by name and arity, every
// argument is converted to its parameter type, and the return value goes back as JSON text.
//
// JSON -> Java: boolean/Boolean (bool), int/long/short/byte and boxed (an integral number in
// range), float/double and boxed (any number), String/CharSequence (string), JSONObject,
// JSONArray, Map (object), List (array), Object (String, Long or Double, Boolean, JSONObject,
// JSONArray, null), String[] int[] long[] double[] boolean[] (arrays); null fits any
// non-primitive. Java -> JSON: null/void, Boolean, Number (integral types stay integers),
// String/CharSequence/Character, JSONObject, JSONArray, JSONObject.NULL, Map, Collection,
// arrays, Enum (its name); anything else is "unsupported_type".
public final class NativeReflection {

    // JSON nesting the bridge accepts (the same limit as the engine side)
    public static final int MAX_DEPTH = 64;

    private NativeReflection() {}

    // A failure with one of the bridge's error codes (bad_args, no_method, unsupported_type, ...)
    public static final class BridgeException extends Exception {
        private static final long serialVersionUID = 1L;
        public final String code;

        public BridgeException(String code, String message) {
            super(message);
            this.code = code;
        }
    }

    // ---------------------------------------------------------------- method lookup

    // the candidates of (class, name, arity, async), sorted for a deterministic overload pick
    private static final Map<String, List<Method>> candidates = new ConcurrentHashMap<>();

    private static String signature(Method m) {
        StringBuilder text = new StringBuilder();
        for (Class<?> type : m.getParameterTypes()) text.append(type.getName()).append(',');
        return text.toString();
    }

    static List<Method> findCandidates(Class<?> cls, String name, int luaArgs, boolean async) {
        final String key = cls.getName() + '#' + name + '#' + luaArgs + (async ? "#a" : "#s");
        List<Method> found = candidates.get(key);
        if (found != null) return found;
        List<Method> list = new ArrayList<>();
        for (Method m : cls.getMethods()) {
            if (!m.getName().equals(name) || !Modifier.isStatic(m.getModifiers())) continue;
            Class<?>[] types = m.getParameterTypes();
            if (async) {
                if (types.length == luaArgs + 1 && types[luaArgs] == NativeCall.class) list.add(m);
            } else if (types.length == luaArgs && !(luaArgs > 0 && types[luaArgs - 1] == NativeCall.class)) {
                list.add(m);
            }
        }
        list.sort(new Comparator<Method>() {
            @Override public int compare(Method a, Method b) {
                return signature(a).compareTo(signature(b));
            }
        });
        candidates.put(key, list);
        return list;
    }

    // a public static method of that name (any arity)
    public static boolean hasMethod(Class<?> cls, String name) {
        for (Method m : cls.getMethods()) {
            if (m.getName().equals(name) && Modifier.isStatic(m.getModifiers())) return true;
        }
        return false;
    }

    // The method and the converted arguments of a call (async: the NativeCall slot left null)
    public static final class Target {
        public final Method method;
        public final Object[] args;
        public final boolean async;

        Target(Method method, Object[] args, boolean async) {
            this.method = method;
            this.args = args;
            this.async = async;
        }
    }

    // Picks the method for `args`: an async call prefers a method with a trailing NativeCall
    // and falls back to a plain one (its return value resolves the call); a sync call only
    // takes plain ones
    public static Target resolve(Class<?> cls, String pluginName, String method, List<Object> args, boolean async) throws BridgeException {
        final int n = args.size();
        BridgeException lastConversion = null;
        for (int pass = 0; pass < 2; pass++) {
            final boolean asyncPass = async && pass == 0;
            if (!async && pass == 1) break;
            for (Method m : findCandidates(cls, method, n, asyncPass)) {
                Class<?>[] types = m.getParameterTypes();
                Object[] converted = new Object[types.length];
                try {
                    for (int i = 0; i < n; i++) converted[i] = convert(args.get(i), types[i], 0);
                } catch (BridgeException e) {
                    lastConversion = e;
                    continue;
                }
                return new Target(m, converted, asyncPass);
            }
        }
        if (lastConversion != null) {
            throw new BridgeException("bad_args", pluginName + "." + method + ": " + lastConversion.getMessage());
        }
        if (!async && !findCandidates(cls, method, n, true).isEmpty()) {
            throw new BridgeException("async_only", pluginName + "." + method + " takes a NativeCall: it is asynchronous, use Native.callAsync");
        }
        if (hasMethod(cls, method)) {
            throw new BridgeException("bad_args", pluginName + "." + method + " has no public static overload for " + n + " argument" + (n == 1 ? "" : "s") +
                (async ? " (plus a trailing NativeCall)" : ""));
        }
        throw new BridgeException("no_method", pluginName + " (" + cls.getName() + ") has no public static method \"" + method + "\"");
    }

    // Runs `target` (a sync method, or an async one with `call` in its last slot); the return
    // value, or a BridgeException "exception" with the Java class and message
    public static Object invoke(Target target, NativeCall call) throws BridgeException {
        Object[] args = target.args;
        if (target.async) args[args.length - 1] = call;
        try {
            return target.method.invoke(null, args);
        } catch (InvocationTargetException e) {
            Throwable cause = e.getCause() != null ? e.getCause() : e;
            throw new BridgeException("exception", describe(cause));
        } catch (IllegalAccessException e) {
            throw new BridgeException("exception", describe(e));
        }
    }

    public static String describe(Throwable t) {
        String message = t.getMessage();
        return t.getClass().getName() + (message != null ? ": " + message : "");
    }

    // ---------------------------------------------------------------- JSON -> Java

    private static String kindOf(Object value) {
        if (value == null) return "null";
        if (value instanceof Boolean) return "a boolean";
        if (value instanceof Long || value instanceof Integer) return "an integer";
        if (value instanceof Number) return "a number";
        if (value instanceof String) return "a string";
        if (value instanceof List) return "an array";
        if (value instanceof Map) return "an object";
        return value.getClass().getSimpleName();
    }

    private static BridgeException mismatch(Object value, Class<?> type) {
        return new BridgeException("bad_args", "cannot pass " + kindOf(value) + " as " + type.getSimpleName());
    }

    private static boolean isIntegral(Object value) {
        if (value instanceof Long || value instanceof Integer || value instanceof Short || value instanceof Byte) return true;
        if (value instanceof Double || value instanceof Float) {
            double d = ((Number) value).doubleValue();
            return !Double.isInfinite(d) && !Double.isNaN(d) && d == Math.rint(d);
        }
        return false;
    }

    private static long integral(Object value, long min, long max, Class<?> type) throws BridgeException {
        if (!(value instanceof Number) || !isIntegral(value)) throw mismatch(value, type);
        double d = ((Number) value).doubleValue();
        long l = (value instanceof Long || value instanceof Integer) ? ((Number) value).longValue() : (long) d;
        if ((value instanceof Double || value instanceof Float) && (d < (double) Long.MIN_VALUE || d >= (double) Long.MAX_VALUE)) {
            throw new BridgeException("bad_args", "the number " + d + " does not fit " + type.getSimpleName());
        }
        if (l < min || l > max) throw new BridgeException("bad_args", "the number " + l + " does not fit " + type.getSimpleName());
        return l;
    }

    // `value` is a parsed JSON value (null, Boolean, Long, Double, String, List, Map)
    static Object convert(Object value, Class<?> type, int depth) throws BridgeException {
        if (value == null) {
            if (type.isPrimitive()) throw new BridgeException("bad_args", "cannot pass null as " + type.getName());
            return null;
        }
        if (type == boolean.class || type == Boolean.class) {
            if (value instanceof Boolean) return value;
            throw mismatch(value, type);
        }
        if (type == int.class || type == Integer.class) return (int) integral(value, Integer.MIN_VALUE, Integer.MAX_VALUE, type);
        if (type == long.class || type == Long.class) return integral(value, Long.MIN_VALUE, Long.MAX_VALUE, type);
        if (type == short.class || type == Short.class) return (short) integral(value, Short.MIN_VALUE, Short.MAX_VALUE, type);
        if (type == byte.class || type == Byte.class) return (byte) integral(value, Byte.MIN_VALUE, Byte.MAX_VALUE, type);
        if (type == double.class || type == Double.class) {
            if (value instanceof Number) return ((Number) value).doubleValue();
            throw mismatch(value, type);
        }
        if (type == float.class || type == Float.class) {
            if (value instanceof Number) return ((Number) value).floatValue();
            throw mismatch(value, type);
        }
        if (type == String.class || type == CharSequence.class) {
            if (value instanceof String) return value;
            throw mismatch(value, type);
        }
        if (type == JSONObject.class) {
            if (value instanceof Map) return toJSONObject((Map<?, ?>) value);
            throw mismatch(value, type);
        }
        if (type == JSONArray.class) {
            if (value instanceof List) return toJSONArray((List<?>) value);
            throw mismatch(value, type);
        }
        if (type == Map.class || type == LinkedHashMap.class) {
            if (value instanceof Map) return value;
            throw mismatch(value, type);
        }
        if (type == List.class || type == Collection.class || type == ArrayList.class) {
            if (value instanceof List) return value;
            throw mismatch(value, type);
        }
        if (type == Object.class) {
            if (value instanceof Map) return toJSONObject((Map<?, ?>) value);
            if (value instanceof List) return toJSONArray((List<?>) value);
            return value;
        }
        if (type.isArray()) {
            if (!(value instanceof List)) throw mismatch(value, type);
            Class<?> element = type.getComponentType();
            if (element != String.class && element != int.class && element != long.class && element != double.class && element != boolean.class) {
                throw new BridgeException("bad_args", "arrays of " + element.getSimpleName() + " are not supported (String[], int[], long[], double[], boolean[])");
            }
            List<?> list = (List<?>) value;
            Object array = Array.newInstance(element, list.size());
            for (int i = 0; i < list.size(); i++) Array.set(array, i, convert(list.get(i), element, depth + 1));
            return array;
        }
        throw new BridgeException("bad_args", "parameters of type " + type.getName() + " are not supported");
    }

    private static Object toOrgJson(Object value) {
        if (value == null) return JSONObject.NULL;
        if (value instanceof Map) return toJSONObject((Map<?, ?>) value);
        if (value instanceof List) return toJSONArray((List<?>) value);
        return value;
    }

    static JSONObject toJSONObject(Map<?, ?> map) {
        JSONObject object = new JSONObject();
        try {
            for (Map.Entry<?, ?> entry : map.entrySet()) object.put(String.valueOf(entry.getKey()), toOrgJson(entry.getValue()));
        } catch (JSONException e) {
            // values come from JSON: always valid
            throw new IllegalStateException(e);
        }
        return object;
    }

    static JSONArray toJSONArray(List<?> list) {
        JSONArray array = new JSONArray();
        for (Object item : list) array.put(toOrgJson(item));
        return array;
    }

    // ---------------------------------------------------------------- JSON text

    // null, Boolean, Long (integers; outside long: Double), Double, String, ArrayList, LinkedHashMap
    public static Object parse(String text) throws BridgeException {
        if (text == null) return null;
        Parser parser = new Parser(text);
        parser.skipSpace();
        Object value = parser.value(0);
        parser.skipSpace();
        if (parser.pos != text.length()) throw new BridgeException("bad_args", "invalid JSON at " + parser.pos);
        return value;
    }

    @SuppressWarnings("unchecked")
    public static List<Object> parseArray(String text) throws BridgeException {
        Object value = parse(text == null || text.isEmpty() ? "[]" : text);
        if (!(value instanceof List)) throw new BridgeException("bad_args", "the arguments are not a JSON array");
        return (List<Object>) value;
    }

    private static final class Parser {
        final String s;
        int pos;

        Parser(String s) {
            this.s = s;
        }

        void skipSpace() {
            while (pos < s.length()) {
                char c = s.charAt(pos);
                if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
                pos++;
            }
        }

        BridgeException error(String what) {
            return new BridgeException("bad_args", "invalid JSON (" + what + ") at " + pos);
        }

        Object value(int depth) throws BridgeException {
            if (depth > MAX_DEPTH) throw new BridgeException("bad_args", "JSON nested deeper than " + MAX_DEPTH);
            if (pos >= s.length()) throw error("end of text");
            char c = s.charAt(pos);
            switch (c) {
                case '{': return object(depth);
                case '[': return array(depth);
                case '"': return string();
                case 't': return literal("true", Boolean.TRUE);
                case 'f': return literal("false", Boolean.FALSE);
                case 'n': return literal("null", null);
                default: return number();
            }
        }

        Object literal(String word, Object value) throws BridgeException {
            if (!s.startsWith(word, pos)) throw error("unexpected character");
            pos += word.length();
            return value;
        }

        Object number() throws BridgeException {
            int start = pos;
            boolean real = false;
            if (pos < s.length() && s.charAt(pos) == '-') pos++;
            while (pos < s.length()) {
                char c = s.charAt(pos);
                if (c >= '0' && c <= '9') pos++;
                else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') { real = true; pos++; }
                else break;
            }
            String text = s.substring(start, pos);
            if (text.isEmpty() || text.equals("-")) throw error("unexpected character");
            try {
                if (!real) {
                    try {
                        return Long.parseLong(text);
                    } catch (NumberFormatException e) {
                        // outside long: a double, as the engine does
                        return Double.parseDouble(text);
                    }
                }
                return Double.parseDouble(text);
            } catch (NumberFormatException e) {
                throw error("bad number");
            }
        }

        String string() throws BridgeException {
            pos++;
            StringBuilder out = new StringBuilder();
            while (true) {
                if (pos >= s.length()) throw error("unterminated string");
                char c = s.charAt(pos++);
                if (c == '"') return out.toString();
                if (c != '\\') {
                    out.append(c);
                    continue;
                }
                if (pos >= s.length()) throw error("unterminated string");
                char e = s.charAt(pos++);
                switch (e) {
                    case '"': out.append('"'); break;
                    case '\\': out.append('\\'); break;
                    case '/': out.append('/'); break;
                    case 'b': out.append('\b'); break;
                    case 'f': out.append('\f'); break;
                    case 'n': out.append('\n'); break;
                    case 'r': out.append('\r'); break;
                    case 't': out.append('\t'); break;
                    case 'u':
                        if (pos + 4 > s.length()) throw error("bad \\u escape");
                        try {
                            out.append((char) Integer.parseInt(s.substring(pos, pos + 4), 16));
                        } catch (NumberFormatException ex) {
                            throw error("bad \\u escape");
                        }
                        pos += 4;
                        break;
                    default: throw error("bad escape");
                }
            }
        }

        List<Object> array(int depth) throws BridgeException {
            pos++;
            List<Object> list = new ArrayList<>();
            skipSpace();
            if (pos < s.length() && s.charAt(pos) == ']') { pos++; return list; }
            while (true) {
                skipSpace();
                list.add(value(depth + 1));
                skipSpace();
                if (pos >= s.length()) throw error("unterminated array");
                char c = s.charAt(pos++);
                if (c == ']') return list;
                if (c != ',') throw error("expected , or ]");
            }
        }

        Map<String, Object> object(int depth) throws BridgeException {
            pos++;
            Map<String, Object> map = new LinkedHashMap<>();
            skipSpace();
            if (pos < s.length() && s.charAt(pos) == '}') { pos++; return map; }
            while (true) {
                skipSpace();
                if (pos >= s.length() || s.charAt(pos) != '"') throw error("expected a key");
                String key = string();
                skipSpace();
                if (pos >= s.length() || s.charAt(pos++) != ':') throw error("expected :");
                skipSpace();
                map.put(key, value(depth + 1));
                skipSpace();
                if (pos >= s.length()) throw error("unterminated object");
                char c = s.charAt(pos++);
                if (c == '}') return map;
                if (c != ',') throw error("expected , or }");
            }
        }
    }

    // ---------------------------------------------------------------- Java -> JSON text

    // `value` as JSON text; BridgeException "unsupported_type" for what JSON cannot hold
    public static String toJson(Object value) throws BridgeException {
        StringBuilder out = new StringBuilder();
        write(out, value, 0);
        return out.toString();
    }

    private static void write(StringBuilder out, Object value, int depth) throws BridgeException {
        if (depth > MAX_DEPTH) throw new BridgeException("unsupported_type", "a value nested deeper than " + MAX_DEPTH);
        if (value == null || value == JSONObject.NULL) {
            out.append("null");
        } else if (value instanceof Boolean) {
            out.append(((Boolean) value) ? "true" : "false");
        } else if (value instanceof Integer || value instanceof Long || value instanceof Short || value instanceof Byte) {
            out.append(((Number) value).longValue());
        } else if (value instanceof BigInteger) {
            BigInteger big = (BigInteger) value;
            if (big.bitLength() < 64) out.append(big.longValue());
            else writeReal(out, big.doubleValue());
        } else if (value instanceof Float) {
            float f = (Float) value;
            if (Float.isNaN(f) || Float.isInfinite(f)) throw new BridgeException("unsupported_type", "NaN and infinities have no JSON form");
            String text = Float.toString(f);
            out.append(text);
        } else if (value instanceof Number) {
            writeReal(out, ((Number) value).doubleValue());
        } else if (value instanceof CharSequence || value instanceof Character) {
            writeString(out, value.toString());
        } else if (value instanceof Enum) {
            writeString(out, ((Enum<?>) value).name());
        } else if (value instanceof JSONObject) {
            JSONObject object = (JSONObject) value;
            out.append('{');
            boolean first = true;
            for (Iterator<String> keys = object.keys(); keys.hasNext();) {
                String key = keys.next();
                if (!first) out.append(',');
                first = false;
                writeString(out, key);
                out.append(':');
                write(out, object.opt(key), depth + 1);
            }
            out.append('}');
        } else if (value instanceof JSONArray) {
            JSONArray array = (JSONArray) value;
            out.append('[');
            for (int i = 0; i < array.length(); i++) {
                if (i > 0) out.append(',');
                write(out, array.opt(i), depth + 1);
            }
            out.append(']');
        } else if (value instanceof Map) {
            out.append('{');
            boolean first = true;
            for (Map.Entry<?, ?> entry : ((Map<?, ?>) value).entrySet()) {
                if (!first) out.append(',');
                first = false;
                writeString(out, String.valueOf(entry.getKey()));
                out.append(':');
                write(out, entry.getValue(), depth + 1);
            }
            out.append('}');
        } else if (value instanceof Collection) {
            out.append('[');
            boolean first = true;
            for (Object item : (Collection<?>) value) {
                if (!first) out.append(',');
                first = false;
                write(out, item, depth + 1);
            }
            out.append(']');
        } else if (value.getClass().isArray()) {
            out.append('[');
            int n = Array.getLength(value);
            for (int i = 0; i < n; i++) {
                if (i > 0) out.append(',');
                write(out, Array.get(value, i), depth + 1);
            }
            out.append(']');
        } else {
            throw new BridgeException("unsupported_type", "a " + value.getClass().getName() + " has no JSON form (return null, Boolean, Number, String, " +
                "JSONObject, JSONArray, Map, List or an array)");
        }
    }

    private static void writeReal(StringBuilder out, double d) throws BridgeException {
        if (Double.isNaN(d) || Double.isInfinite(d)) throw new BridgeException("unsupported_type", "NaN and infinities have no JSON form");
        // Double.toString keeps the decimal point ("7.0"), so the engine reads a float back
        out.append(Double.toString(d));
    }

    private static void writeString(StringBuilder out, String text) {
        out.append('"');
        for (int i = 0; i < text.length(); i++) {
            char c = text.charAt(i);
            switch (c) {
                case '"': out.append("\\\""); break;
                case '\\': out.append("\\\\"); break;
                case '\n': out.append("\\n"); break;
                case '\r': out.append("\\r"); break;
                case '\t': out.append("\\t"); break;
                case '\b': out.append("\\b"); break;
                case '\f': out.append("\\f"); break;
                default:
                    if (c < 0x20) out.append(String.format("\\u%04x", (int) c));
                    else out.append(c);
            }
        }
        out.append('"');
    }

    // ---------------------------------------------------------------- for tests

    static void clearCache() {
        candidates.clear();
    }

    static List<String> describeCandidates(Class<?> cls, String name, int n, boolean async) {
        List<String> out = new ArrayList<>();
        for (Method m : findCandidates(cls, name, n, async)) out.add(Arrays.toString(m.getParameterTypes()));
        return out;
    }
}
