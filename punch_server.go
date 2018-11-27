package main

import (
	"bytes"
	"encoding/binary"
	"errors"
	"io"
	"math/rand"
	"net"
	"os"
	"sync"
	"time"

	log "github.com/sirupsen/logrus"
)

type PeerInfo struct {
	IP      [16]byte
	Port    uint16
	NatType uint16
	Meta    string
	ID      uint32
}

type natInfo struct {
	IP      [16]byte
	Port    uint16
	NatType uint16
}

const (
	_ = iota
	Enroll
	GetPeerInfo
	NotifyPeer
	GetPeerInfoFromMeta
	NotifyPeerFromMeta

	PeerOffline = 1
	PeerError   = 2

	ListeningPort = ":9988"
)

var (
	seq              uint32 = 1
	peers            map[uint32]PeerInfo
	peersFromMeta    map[string]PeerInfo
	peerConn         map[uint32]net.Conn
	peerConnFromMeta map[string]net.Conn
	mutex            sync.RWMutex
	letterRunes      = []rune("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ")
	ErrPeerNotFound  = errors.New("Peer not found")
	ErrConnNotFound  = errors.New("Connection not found, peer maybe is now offline")
)

func init() {
	peers = make(map[uint32]PeerInfo)
	peersFromMeta = make(map[string]PeerInfo)
	peerConn = make(map[uint32]net.Conn)
	peerConnFromMeta = make(map[string]net.Conn)
	rand.Seed(time.Now().UnixNano())

	log.SetOutput(os.Stdout)
	log.SetLevel(log.DebugLevel)
}

func main() {
	l, err := net.Listen("tcp", ListeningPort)
	if err != nil {
		log.WithFields(log.Fields{
			"err": err,
		}).Fatal("Unable to start server")
	}
	defer l.Close()
	go dumpPeers()
	for {
		conn, err := l.Accept()
		if err != nil {
			log.WithFields(log.Fields{
				"err": err,
			}).Error("Accepting connection failed")
			continue
		}
		log.Info("New connection received")
		go handleConn(conn)
	}
}

func dumpPeers() {
	for {
		time.Sleep(10 * time.Second)
		mutex.RLock()
		for k, v := range peers {
			log.WithFields(log.Fields{
				"Key":     k,
				"ID":      v.ID,
				"Meta":    v.Meta,
				"IP":      string(v.IP[:]),
				"Port":    v.Port,
				"NatType": v.NatType,
			}).Debug("peer info")
		}
		mutex.RUnlock()
	}
}

func RandStringRunes(n int) string {
	b := make([]rune, n)
	for i := range b {
		b[i] = letterRunes[rand.Intn(len(letterRunes))]
	}
	return string(b)
}

func readMeta(c io.Reader) (meta string, err error) {
	var metaSize uint8
	err = binary.Read(c, binary.BigEndian, &metaSize)
	if err != nil || metaSize == 0 {
		log.WithFields(log.Fields{
			"err":      err,
			"metaSize": metaSize,
		}).Info("reading meta failed")
		return
	}
	data := make([]byte, metaSize)
	if _, err = c.Read(data); err != nil {
		log.WithFields(log.Fields{
			"err": err,
		}).Info("reading meta failed")
		return
	}
	meta = string(data)
	return
}

func writeMeta(c io.Writer, meta string) (err error) {
	err = binary.Write(c, binary.BigEndian, uint8(len(meta)))
	if err != nil {
		return
	}
	return binary.Write(c, binary.BigEndian, []byte(meta))
}

func readPeerInfo(r io.Reader) (p PeerInfo, err error) {
	var IP [16]byte
	var Port uint16
	var NatType uint16
	if err = binary.Read(r, binary.BigEndian, &IP); err != nil {
		return
	}
	if err = binary.Read(r, binary.BigEndian, &Port); err != nil {
		return
	}
	if err = binary.Read(r, binary.BigEndian, &NatType); err != nil {
		return
	}
	p = PeerInfo{
		IP:      IP,
		Port:    Port,
		NatType: NatType,
	}

	meta, err := readMeta(r)
	if err != nil {
		log.WithFields(log.Fields{
			"err": err,
		}).Warn("reading meta failed")
		meta = RandStringRunes(18)
	}
	p.Meta = meta
	log.WithFields(log.Fields{
		"meta": meta,
	}).Info("reading meta succeeded")
	return
}

func writePeerInfo(w io.Writer, p PeerInfo) (err error) {
	p1 := natInfo{
		IP:      p.IP,
		Port:    p.Port,
		NatType: p.NatType,
	}
	var buf bytes.Buffer
	if err = binary.Write(&buf, binary.BigEndian, p.ID); err != nil {
		return
	}
	if err = binary.Write(&buf, binary.BigEndian, p1); err != nil {
		return
	}
	if err = writeMeta(&buf, p.Meta); err != nil {
		return
	}
	return binary.Write(w, binary.BigEndian, (&buf).Bytes())
}

func getPeerInfo(p PeerInfo) (p1 PeerInfo, err error) {
	var ok bool
	mutex.RLock()
	defer mutex.RUnlock()
	if p.ID != 0 {
		if p1, ok = peers[p.ID]; ok {
			return
		}
	}
	if p.Meta != "" {
		if p1, ok = peersFromMeta[p.Meta]; ok {
			return
		}
	}
	err = ErrPeerNotFound
	return
}

func getConn(p PeerInfo) (c net.Conn, err error) {
	var ok bool
	mutex.RLock()
	defer mutex.RUnlock()
	if p.ID != 0 {
		if c, ok = peerConn[p.ID]; ok {
			return
		}
	}
	if p.Meta != "" {
		if c, ok = peerConnFromMeta[p.Meta]; ok {
			return
		}
	}
	err = ErrConnNotFound
	return
}

// 2 bytes for message type
func handleConn(c net.Conn) {
	defer c.Close()
	log.Info("new connection received!")
	var myInfo PeerInfo
	for {
		var myBuf bytes.Buffer
		w := io.MultiWriter(&myBuf, c)
		data := make([]byte, 2)
		_, err := c.Read(data)
		log.WithFields(log.Fields{
			"header": data,
		}).Info("new received header")
		if err != nil {
			mutex.Lock()
			delete(peers, myInfo.ID)
			delete(peerConn, myInfo.ID)
			delete(peersFromMeta, myInfo.Meta)
			delete(peerConnFromMeta, myInfo.Meta)
			mutex.Unlock()
			log.WithFields(log.Fields{
				"err":  err,
				"myID": myInfo.ID,
			}).Info("peer left")
			return
		}
		t := binary.BigEndian.Uint16(data[:])
		switch t {
		case Enroll:
			var err error
			myInfo, err = readPeerInfo(c)
			if err != nil {
				log.WithFields(log.Fields{
					"err": err,
				}).Warn("Reading meta failed")
				break
			}
			mutex.Lock()
			seq++
			myInfo.ID = seq
			peers[myInfo.ID] = myInfo
			peerConn[myInfo.ID] = c
			if myInfo.Meta != "" {
				peersFromMeta[myInfo.Meta] = myInfo
				peerConnFromMeta[myInfo.Meta] = c
			}
			mutex.Unlock()
			log.WithFields(log.Fields{
				"ID":      myInfo.ID,
				"Meta":    myInfo.Meta,
				"IP":      string(myInfo.IP[:]),
				"Port":    myInfo.Port,
				"NatType": myInfo.NatType,
			}).Debug("New peer enrolled")
			err = binary.Write(w, binary.BigEndian, myInfo.ID)
			if err != nil {
				log.WithFields(log.Fields{
					"err":     err,
					"ID":      myInfo.ID,
					"Meta":    myInfo.Meta,
					"IP":      string(myInfo.IP[:]),
					"Port":    myInfo.Port,
					"NatType": myInfo.NatType,
				}).Warn("Unable to return my peer info")
				break
			}
		case GetPeerInfo:
			var peerID uint32
			err = binary.Read(c, binary.BigEndian, &peerID)
			if err != nil {
				log.WithFields(log.Fields{
					"err":    err,
					"peerID": peerID,
					"myID":   myInfo.ID,
				}).Warn("Unable to get peer id")
				binary.Write(c, binary.BigEndian, PeerOffline)
				break
			}
			peer, err := getPeerInfo(PeerInfo{ID: peerID})
			if err != nil {
				log.WithFields(log.Fields{
					"err":    err,
					"peerID": peerID,
					"myID":   myInfo.ID,
				}).Warn("Unable to get peer info")
				break
			}
			err = writePeerInfo(w, peer)
			if err != nil {
				log.WithFields(log.Fields{
					"err":    err,
					"peerID": peerID,
					"myID":   myInfo.ID,
				}).Warn("Unable to write peer info")
				break
			}
		case NotifyPeer:
			var peerID uint32
			err = binary.Read(c, binary.BigEndian, &peerID)
			if err != nil {
				log.WithFields(log.Fields{
					"err":    err,
					"peerID": peerID,
					"myID":   myInfo.ID,
				}).Warn("Unable to get peer id")
				break
			}
			conn, err := getConn(PeerInfo{ID: peerID})
			if err != nil {
				log.WithFields(log.Fields{
					"err":    err,
					"peerID": peerID,
					"myID":   myInfo.ID,
				}).Warn("Unable to get peer conn")
				binary.Write(c, binary.BigEndian, PeerOffline)
				break
			}
			err = writePeerInfo(conn, myInfo)
			if err != nil {
				log.WithFields(log.Fields{
					"err":    err,
					"peerID": peerID,
					"myID":   myInfo.ID,
				}).Warn("Unable to write my peer info to peer connection")
				break
			}
		case GetPeerInfoFromMeta:
			peerMeta, err := readMeta(c)
			if err != nil {
				log.WithFields(log.Fields{
					"err":    err,
					"myMeta": myInfo.Meta,
				}).Warn("Unable to get peer meta")
				break
			}
			peer, err := getPeerInfo(PeerInfo{Meta: peerMeta})
			if err != nil {
				log.WithFields(log.Fields{
					"err":    err,
					"meta":   peerMeta,
					"myMeta": myInfo.Meta,
				}).Warn("Unable to get peer info")
				break
			}
			err = writePeerInfo(w, peer)
			if err != nil {
				log.WithFields(log.Fields{
					"err":      err,
					"peerMeta": peer.Meta,
					"myMeta":   myInfo.Meta,
				}).Warn("Unable to write peer info")
				break
			}
		case NotifyPeerFromMeta:
			peerMeta, err := readMeta(c)
			if err != nil {
				log.WithFields(log.Fields{
					"err":    err,
					"myMeta": myInfo.Meta,
				}).Warn("Unable to get peer id")
				break
			}
			conn, err := getConn(PeerInfo{Meta: peerMeta})
			if err != nil {
				log.WithFields(log.Fields{
					"err":    err,
					"meta":   peerMeta,
					"myMeta": myInfo.Meta,
				}).Warn("Unable to get peer conn")
				binary.Write(c, binary.BigEndian, PeerOffline)
				break
			}
			err = writePeerInfo(conn, myInfo)
			if err != nil {
				log.WithFields(log.Fields{
					"err":      err,
					"peerMeta": peerMeta,
					"myMeta":   myInfo.Meta,
				}).Warn("Unable to write my peer to peer connection")
				break
			}
		default:
			log.WithFields(log.Fields{
				"type": t,
			}).Warn("Illegal message")
		}
		log.WithFields(log.Fields{
			"response": (&myBuf).Bytes(),
		}).Debug("Response sent")
	}

	return
}
