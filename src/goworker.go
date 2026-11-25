package main

import (
	"bufio"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"os"
	"reflect"
	"strings"
	"sync"

	"github.com/traefik/yaegi/interp"
	"github.com/traefik/yaegi/stdlib"
)

type request struct {
	ID   int    `json:"id"`
	Op   string `json:"op"`
	Code string `json:"code"`
}

type reply struct {
	ID    int    `json:"id"`
	OK    bool   `json:"ok"`
	Err   string `json:"err"`
	Value string `json:"value"`
	Has   bool   `json:"has"`
}

type outMsg struct {
	Kind string `json:"kind"`
	Text string `json:"text"`
}

var (
	out    *bufio.Writer
	in     = bufio.NewReader(os.Stdin)
	writeM sync.Mutex
)

func send(v interface{}) {
	b, err := json.Marshal(v)
	if err != nil {
		return
	}
	writeM.Lock()
	out.Write(b)
	out.WriteByte('\n')
	out.Flush()
	writeM.Unlock()
}

func b64(s string) string { return base64.StdEncoding.EncodeToString([]byte(s)) }

func unb64(s string) string {
	d, err := base64.StdEncoding.DecodeString(s)
	if err != nil {
		return ""
	}
	return string(d)
}

func uniReq(op, key string, rest ...string) (string, string) {
	line := "@@uni\t" + op + "\t" + b64(key)
	for _, r := range rest {
		line += "\t" + r
	}
	writeM.Lock()
	out.WriteString(line)
	out.WriteByte('\n')
	out.Flush()
	writeM.Unlock()
	resp, err := in.ReadString('\n')
	if err != nil {
		panic("uni: host closed")
	}
	f := strings.Split(strings.TrimRight(resp, "\r\n"), "\t")
	if len(f) > 1 && f[0] == "@@e" {
		panic(unb64(f[1]))
	}
	if len(f) < 3 {
		return "n", ""
	}
	return f[1], unb64(f[2])
}

type Uni struct{}

func (Uni) SetInt(name string, v int) {
	uniReq("set", name, "i", b64(fmt.Sprintf("%d", v)))
}

func (Uni) SetStr(name string, v string) { uniReq("set", name, "s", b64(v)) }

func (Uni) SetFloat(name string, v float64) {
	uniReq("set", name, "f", b64(fmt.Sprintf("%v", v)))
}

func (Uni) SetBool(name string, v bool) {
	t := "0"
	if v {
		t = "1"
	}
	uniReq("set", name, "b", b64(t))
}

func (Uni) SetBig(name string, v string) { uniReq("set", name, "i", b64(v)) }

func (Uni) GetInt(name string) int {
	_, v := uniReq("get", name)
	var n int
	fmt.Sscanf(v, "%d", &n)
	return n
}

func (Uni) GetFloat(name string) float64 {
	_, v := uniReq("get", name)
	var f float64
	fmt.Sscanf(v, "%g", &f)
	return f
}

func (Uni) GetStr(name string) string {
	_, v := uniReq("get", name)
	return v
}

func (Uni) GetBool(name string) bool {
	t, v := uniReq("get", name)
	if t == "b" {
		return v == "1"
	}
	return v != "" && v != "0" && v != "0.0"
}

func (Uni) Call(name string, args ...string) string {
	parts := make([]string, len(args))
	for i, a := range args {
		parts[i] = b64(a)
	}
	_, v := uniReq("call", name, parts...)
	return v
}

type sink struct{ buf strings.Builder }

func (s *sink) Write(p []byte) (int, error) {
	s.buf.Write(p)
	return len(p), nil
}

func (s *sink) flush() {
	if s.buf.Len() == 0 {
		return
	}
	text := s.buf.String()
	s.buf.Reset()
	send(outMsg{Kind: "out", Text: text})
}

func valueText(rv reflect.Value) (string, bool) {
	if !rv.IsValid() {
		return "", false
	}
	switch rv.Kind() {
	case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		return fmt.Sprintf("%d", rv.Int()), true
	case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64:
		return fmt.Sprintf("%d", rv.Uint()), true
	case reflect.Float32, reflect.Float64:
		return fmt.Sprintf("%v", rv.Float()), true
	case reflect.String:
		return rv.String(), true
	case reflect.Bool:
		if rv.Bool() {
			return "true", true
		}
		return "false", true
	}
	if rv.Kind() == reflect.Ptr && rv.IsNil() {
		return "", false
	}
	return fmt.Sprintf("%v", rv.Interface()), true
}

func main() {
	out = bufio.NewWriter(os.Stdout)
	sk := &sink{}
	i := interp.New(interp.Options{Stdout: sk, Stderr: sk})
	_ = i.Use(stdlib.Symbols)
	_ = i.Use(interp.Exports{
		"uniland/uni/uni": {
			"Uni": reflect.ValueOf(Uni{}),
		},
	})
	for _, pkg := range []string{"fmt", "strings", "strconv", "math", "sort", "time",
		"math/big", "unicode", "regexp"} {
		if _, err := i.Eval(`import "` + pkg + `"`); err != nil {
			send(reply{OK: false, Err: err.Error()})
		}
	}
	if _, err := i.Eval(`import uniapi "uniland/uni"`); err != nil {
		send(reply{OK: false, Err: err.Error()})
	}
	if _, err := i.Eval(`var uni = uniapi.Uni`); err != nil {
		send(reply{OK: false, Err: err.Error()})
	}
	for {
		line, err := in.ReadString('\n')
		if err != nil && line == "" {
			return
		}
		var req request
		if json.Unmarshal([]byte(strings.TrimRight(line, "\r\n")), &req) != nil {
			continue
		}
		if req.Op == "shutdown" {
			return
		}
		res := reply{ID: req.ID, OK: true}
		func() {
			defer func() {
				if r := recover(); r != nil {
					res.OK = false
					res.Err = fmt.Sprint(r)
				}
			}()
			if req.Op == "eval" {
				rv, err := i.Eval(req.Code)
				if err != nil {
					res.OK = false
					res.Err = err.Error()
					return
				}
				if text, has := valueText(rv); has {
					res.Value = text
					res.Has = true
				}
				return
			}
			body := strings.TrimSpace(req.Code)
			if strings.HasPrefix(body, "func ") {
				openAt := strings.Index(body, "{")
				closeAt := strings.LastIndex(body, "}")
				if openAt > 0 && closeAt > openAt {
					name := strings.TrimSpace(body[:openAt])
					name = strings.TrimPrefix(name, "func ")
					inner := body[openAt+1 : closeAt]
					wrapped := "func " + name + "() {\n" + inner + "\n}"
					if _, err := i.Eval(wrapped); err != nil {
						res.OK = false
						res.Err = err.Error()
					}
					return
				}
			}
			if _, err := i.Eval("{};\n" + body); err != nil {
				res.OK = false
				res.Err = err.Error()
			}
		}()
		sk.flush()
		send(res)
	}
}
