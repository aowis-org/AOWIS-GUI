// Test-only peer for the GUI catalogue and health checks.
package main

import (
	"bufio"
	"crypto/ecdh"
	"crypto/rand"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"net/http"
	"net/http/httputil"
	"net/netip"
	"net/url"
	"os"
	"strconv"
	"strings"
	"time"

	"golang.zx2c4.com/wireguard/conn"
	"golang.zx2c4.com/wireguard/device"
	"golang.zx2c4.com/wireguard/tun/netstack"
)

const (
	serverIP = "10.173.91.1"
	clientIP = "10.173.91.2"
	httpPort = 18080
)

type keys struct {
	privateHex string
	privateB64 string
	publicHex  string
	publicB64  string
}

func makeKeys() (keys, error) {
	key, err := ecdh.X25519().GenerateKey(rand.Reader)
	if err != nil {
		return keys{}, err
	}
	return keys{
		privateHex: hex.EncodeToString(key.Bytes()),
		privateB64: base64.StdEncoding.EncodeToString(key.Bytes()),
		publicHex:  hex.EncodeToString(key.PublicKey().Bytes()),
		publicB64:  base64.StdEncoding.EncodeToString(key.PublicKey().Bytes()),
	}, nil
}

// Do not log the generated client private key. It is sent only to the parent
// test process via a private stdout pipe; the keys are ephemeral per run.
type readyMessage struct {
	Endpoint         string `json:"endpoint"`
	ServerPublicKey  string `json:"server_public_key"`
	ClientPrivateKey string `json:"client_private_key"`
}

func run() error {
	serverKeys, err := makeKeys()
	if err != nil {
		return err
	}
	clientKeys, err := makeKeys()
	if err != nil {
		return err
	}
	tun, network, err := netstack.CreateNetTUN([]netip.Addr{netip.MustParseAddr(serverIP)}, nil, 1420)
	if err != nil {
		return fmt.Errorf("userspace server network: %w", err)
	}
	dev := device.NewDevice(tun, conn.NewDefaultBind(),
		device.NewLogger(device.LogLevelError, "aowis-qt-peer: "))
	defer dev.Close()
	ipc := fmt.Sprintf("private_key=%s\nlisten_port=0\nreplace_peers=true\npublic_key=%s\nallowed_ip=%s/32\n",
		serverKeys.privateHex, clientKeys.publicHex, clientIP)
	if err := dev.IpcSet(ipc); err != nil {
		return fmt.Errorf("peer configuration: %w", err)
	}
	if err := dev.Up(); err != nil {
		return fmt.Errorf("peer startup: %w", err)
	}
	status, err := dev.IpcGet()
	if err != nil {
		return fmt.Errorf("peer status: %w", err)
	}
	port := 0
	for _, line := range strings.Split(status, "\n") {
		if value, ok := strings.CutPrefix(line, "listen_port="); ok {
			port, err = strconv.Atoi(value)
			if err != nil {
				return errors.New("invalid UDP port reported by WireGuard")
			}
			break
		}
	}
	if port < 1 || port > 65535 {
		return errors.New("WireGuard UDP port not assigned")
	}
	listener, err := network.ListenTCP(&net.TCPAddr{Port: httpPort})
	if err != nil {
		return fmt.Errorf("private HTTP listener: %w", err)
	}
	// The GUI fixture owns the fake DVMDB. Expose it through the peer's
	// private netstack and forward only to the fixture's loopback port.
	if len(os.Args) != 2 {
		return errors.New("expected one fixture port")
	}
	fixturePort, err := strconv.Atoi(os.Args[1])
	if err != nil || fixturePort < 1 || fixturePort > 65535 {
		return errors.New("invalid fixture port")
	}
	target := &url.URL{Scheme: "http", Host: net.JoinHostPort("127.0.0.1", strconv.Itoa(fixturePort))}
	proxy := httputil.NewSingleHostReverseProxy(target)
	httpServer := &http.Server{Handler: proxy, ReadHeaderTimeout: 3 * time.Second}
	defer httpServer.Close()
	serveError := make(chan error, 1)
	go func() {
		if err := httpServer.Serve(listener); err != nil && err != http.ErrServerClosed {
			serveError <- err
		}
	}()

	// Readiness means the peer UDP socket and the in-netstack HTTP listener
	// have both been created, not merely that this process has started.
	if err := json.NewEncoder(os.Stdout).Encode(readyMessage{
		Endpoint:         net.JoinHostPort("127.0.0.1", strconv.Itoa(port)),
		ServerPublicKey:  serverKeys.publicB64,
		ClientPrivateKey: clientKeys.privateB64,
	}); err != nil {
		return fmt.Errorf("readiness output: %w", err)
	}
	commands := bufio.NewScanner(os.Stdin)
	for commands.Scan() {
		if commands.Text() == "quit" {
			return nil
		}
		select {
		case err := <-serveError:
			return fmt.Errorf("HTTP service: %w", err)
		default:
		}
	}
	return commands.Err()
}

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "test peer failed:", err)
		os.Exit(1)
	}
}
