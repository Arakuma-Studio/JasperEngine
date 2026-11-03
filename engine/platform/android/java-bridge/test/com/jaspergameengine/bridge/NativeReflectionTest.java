package com.jaspergameengine.bridge;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Before;
import org.junit.Test;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

// NativeReflection on the plain JVM (./gradlew :jasperbridge:testDebugUnitTest): method choice,
// every conversion of the spec (6.5), Kotlin's @JvmStatic shape and unsupported values
public class NativeReflectionTest {

    // ---- plugin classes as games write them

    public static class Plugin {
        public static int answer() { return 42; }
        public static String greet(String name) { return "hi " + name; }
        public static long add(long a, long b) { return a + b; }
        public static double half(double x) { return x / 2; }
        public static boolean not(boolean b) { return !b; }
        public static byte smallByte(byte b) { return b; }
        public static short smallShort(short s) { return s; }
        public static float flt(float f) { return f; }
        public static Integer boxed(Integer i) { return i; }
        public static String nullable(String s) { return s == null ? "was null" : s; }
        public static CharSequence chars(CharSequence s) { return s; }
        public static int keys(JSONObject o) { return o.length(); }
        public static int count(JSONArray a) { return a.length(); }
        public static Object echo(Object o) { return o; }
        public static Map<String, Object> map(Map<String, Object> m) { return m; }
        public static List<Object> list(List<Object> l) { return l; }
        public static int sumInts(int[] values) { int s = 0; for (int v : values) s += v; return s; }
        public static double sumDoubles(double[] values) { double s = 0; for (double v : values) s += v; return s; }
        public static String joined(String[] values) { return String.join("+", values); }
        public static void nothing() {}
        public static Object thrower() { throw new IllegalStateException("broken"); }
        public static Object weird() { return new Thread(); }
        public static Object nan() { return Double.NaN; }
        public static Object seven() { return 7.0; }
        public static Object nested() {
            Map<String, Object> m = new LinkedHashMap<>();
            m.put("list", Arrays.asList(1, "two", null, true));
            m.put("array", new int[]{3, 4});
            m.put("null", JSONObject.NULL);
            m.put("enum", Thread.State.NEW);
            return m;
        }
        // overloads of one arity: the first (sorted by parameter types) that converts wins
        public static String over(String s) { return "string"; }
        public static String over(int i) { return "int"; }
        public static String over(boolean b) { return "boolean"; }
        // async
        public static void later(String x, NativeCall call) { call.resolve(x); }
        public static void onlyAsync(NativeCall call) {}
        // not static: never a plugin method
        public int instance() { return 1; }
    }

    // what Kotlin's `object K { @JvmStatic fun f(x: Int) = x * 3 }` compiles to
    public static final class KotlinObject {
        public static final KotlinObject INSTANCE = new KotlinObject();
        private KotlinObject() {}
        public static int f(int x) { return INSTANCE.fImpl(x); }
        public final int fImpl(int x) { return x * 3; }
    }

    @Before
    public void clear() {
        NativeReflection.clearCache();
    }

    private static Object sync(Class<?> cls, String method, String argsJson) throws NativeReflection.BridgeException {
        List<Object> args = NativeReflection.parseArray(argsJson);
        NativeReflection.Target target = NativeReflection.resolve(cls, cls.getSimpleName(), method, args, false);
        return NativeReflection.invoke(target, null);
    }

    private static String syncJson(String method, String argsJson) throws NativeReflection.BridgeException {
        return NativeReflection.toJson(sync(Plugin.class, method, argsJson));
    }

    private static String code(Class<?> cls, String method, String argsJson) {
        try {
            sync(cls, method, argsJson);
        } catch (NativeReflection.BridgeException e) {
            return e.code;
        }
        fail("expected a failure of " + method);
        return null;
    }

    @Test
    public void primitivesAndBoxes() throws Exception {
        assertEquals("42", syncJson("answer", "[]"));
        assertEquals("\"hi ann\"", syncJson("greet", "[\"ann\"]"));
        assertEquals("5", syncJson("add", "[2, 3]"));
        assertEquals("2.5", syncJson("half", "[5]"));
        assertEquals("false", syncJson("not", "[true]"));
        assertEquals("-7", syncJson("smallByte", "[-7]"));
        assertEquals("300", syncJson("smallShort", "[300]"));
        assertEquals("1.5", syncJson("flt", "[1.5]"));
        assertEquals("9", syncJson("boxed", "[9]"));
        assertEquals("null", syncJson("boxed", "[null]"));
        assertEquals("\"was null\"", syncJson("nullable", "[null]"));
        assertEquals("\"x\"", syncJson("chars", "[\"x\"]"));
        // an integral float is an integer argument; a fractional one is not
        assertEquals("5", syncJson("add", "[2.0, 3]"));
        assertEquals("bad_args", code(Plugin.class, "add", "[2.5, 3]"));
        // out of range
        assertEquals("bad_args", code(Plugin.class, "smallByte", "[300]"));
        assertEquals("bad_args", code(Plugin.class, "boxed", "[3000000000]"));
        // wrong kinds
        assertEquals("bad_args", code(Plugin.class, "not", "[1]"));
        assertEquals("bad_args", code(Plugin.class, "greet", "[1]"));
        assertEquals("bad_args", code(Plugin.class, "half", "[null]"));
    }

    @Test
    public void objectsArraysAndNatural() throws Exception {
        assertEquals("2", syncJson("keys", "[{\"a\": 1, \"b\": [1]}]"));
        assertEquals("3", syncJson("count", "[[1, 2, 3]]"));
        assertEquals("{\"a\":[1,2.5,null,\"s\",false]}", syncJson("map", "[{\"a\": [1, 2.5, null, \"s\", false]}]"));
        assertEquals("[1,{\"b\":2}]", syncJson("list", "[[1, {\"b\": 2}]]"));
        assertEquals("6", syncJson("sumInts", "[[1, 2, 3]]"));
        assertEquals("1.5", syncJson("sumDoubles", "[[1, 0.5]]"));
        assertEquals("\"a+b\"", syncJson("joined", "[[\"a\", \"b\"]]"));
        // Object: String, Long or Double, Boolean, JSONObject, JSONArray, null
        Object o = sync(Plugin.class, "echo", "[3]");
        assertEquals(Long.valueOf(3), o);
        o = sync(Plugin.class, "echo", "[3.5]");
        assertEquals(Double.valueOf(3.5), o);
        assertTrue(sync(Plugin.class, "echo", "[{\"k\": 1}]") instanceof JSONObject);
        assertTrue(sync(Plugin.class, "echo", "[[1]]") instanceof JSONArray);
        assertNull(sync(Plugin.class, "echo", "[null]"));
        assertEquals("{\"k\":[1,null]}", syncJson("echo", "[{\"k\": [1, null]}]"));
        // integers outside long become doubles; unicode survives
        assertEquals(Double.valueOf(1e20), sync(Plugin.class, "echo", "[100000000000000000000]"));
        assertEquals("\"hé 😀\"", syncJson("echo", "[\"h\\u00e9 \\ud83d\\ude00\"]"));
    }

    @Test
    public void returnsToJson() throws Exception {
        assertEquals("null", syncJson("nothing", "[]"));
        // a double keeps its point, so the engine reads a float
        assertEquals("7.0", syncJson("seven", "[]"));
        assertEquals("{\"list\":[1,\"two\",null,true],\"array\":[3,4],\"null\":null,\"enum\":\"NEW\"}", syncJson("nested", "[]"));
        assertEquals("\"a\\\"b\\\\c\\n\\u0001\"", NativeReflection.toJson("a\"b\\c\n\u0001"));
        try {
            syncJson("weird", "[]");
            fail("a Thread has no JSON form");
        } catch (NativeReflection.BridgeException e) {
            assertEquals("unsupported_type", e.code);
            assertTrue(e.getMessage().contains("java.lang.Thread"));
        }
        try {
            syncJson("nan", "[]");
            fail("NaN has no JSON form");
        } catch (NativeReflection.BridgeException e) {
            assertEquals("unsupported_type", e.code);
        }
        // too deep
        List<Object> deep = new ArrayList<>();
        List<Object> cur = deep;
        for (int i = 0; i < 70; i++) {
            List<Object> next = new ArrayList<>();
            cur.add(next);
            cur = next;
        }
        try {
            NativeReflection.toJson(deep);
            fail("too deep");
        } catch (NativeReflection.BridgeException e) {
            assertEquals("unsupported_type", e.code);
        }
    }

    @Test
    public void overloadsAndErrors() throws Exception {
        // sorted by parameter type names: boolean, int, java.lang.String
        assertEquals(Arrays.asList("[boolean]", "[int]", "[class java.lang.String]"), NativeReflection.describeCandidates(Plugin.class, "over", 1, false));
        assertEquals("\"string\"", syncJson("over", "[\"x\"]"));
        assertEquals("\"int\"", syncJson("over", "[5]"));
        assertEquals("\"boolean\"", syncJson("over", "[true]"));
        assertEquals("bad_args", code(Plugin.class, "over", "[[1]]"));
        assertEquals("bad_args", code(Plugin.class, "greet", "[]"));
        assertEquals("no_method", code(Plugin.class, "missing", "[]"));
        assertEquals("no_method", code(Plugin.class, "instance", "[]"));
        assertEquals("async_only", code(Plugin.class, "onlyAsync", "[]"));
        try {
            sync(Plugin.class, "thrower", "[]");
            fail("expected the exception");
        } catch (NativeReflection.BridgeException e) {
            assertEquals("exception", e.code);
            assertEquals("java.lang.IllegalStateException: broken", e.getMessage());
        }
        assertTrue(NativeReflection.hasMethod(Plugin.class, "greet"));
        assertFalse(NativeReflection.hasMethod(Plugin.class, "instance"));
        assertEquals("bad_args", code(Plugin.class, "answer", "{\"not\": \"an array\"}"));
        assertEquals("bad_args", code(Plugin.class, "answer", "[1,"));
    }

    @Test
    public void asyncTargets() throws Exception {
        List<Object> args = NativeReflection.parseArray("[\"v\"]");
        NativeReflection.Target target = NativeReflection.resolve(Plugin.class, "Plugin", "later", args, true);
        assertTrue(target.async);
        assertEquals(2, target.args.length);
        // a plain method serves an async call too (its return value resolves it)
        target = NativeReflection.resolve(Plugin.class, "Plugin", "greet", args, true);
        assertFalse(target.async);
        assertEquals("hi v", NativeReflection.invoke(target, null));
        // NativeCall.getArgs is the Lua arguments as a JSONArray
        NativeCall call = new NativeCall(5, "Plugin", "later", NativeReflection.parseArray("[1, {\"a\": null}]"));
        assertEquals(2, call.getArgs().length());
        assertEquals(5, call.getId());
        assertFalse(call.isCancelled());
        call.markCancelled();
        assertTrue(call.isCancelled());
    }

    @Test
    public void kotlinJvmStatic() throws Exception {
        assertEquals("21", NativeReflection.toJson(sync(KotlinObject.class, "f", "[7]")));
        // the instance method of the object is no plugin method
        assertEquals("no_method", code(KotlinObject.class, "fImpl", "[7]"));
    }

    @Test
    public void parserKeepsIntegersAndDoubles() throws Exception {
        List<Object> v = NativeReflection.parseArray("[1, -2, 3.0, 1e3, \"s\", true, null, {}, []]");
        assertEquals(Long.valueOf(1), v.get(0));
        assertEquals(Long.valueOf(-2), v.get(1));
        assertEquals(Double.valueOf(3.0), v.get(2));
        assertEquals(Double.valueOf(1000.0), v.get(3));
        assertTrue(v.get(7) instanceof Map);
        assertTrue(v.get(8) instanceof List);
        assertArrayEquals(new Object[]{"s", true, null}, new Object[]{v.get(4), v.get(5), v.get(6)});
    }
}
