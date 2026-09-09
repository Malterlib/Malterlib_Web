// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import express from "express";

import https from "https";
import http from "http";
let fs = require("fs");
//let process = require("process");

const app = express();

function timeout(ms: number) {
    return new Promise(resolve => setTimeout(resolve, ms));
}

app.get("/", (req, res) => {
    res.send("Root Reply");
})

app.get("/slow-request", async (req, res) => {
	await timeout(2000);
    res.send("Slow Reply");
})

app.post("/echo", (req, res) => {
    req.pipe(res);
});

app.get("/redirect", (req, res) => {
    res.redirect(`http://localhost:${req.socket.localPort}/`);
});

const tcpServer = http.createServer(app);
tcpServer.listen(0, "127.0.0.1", () => {
    const address = tcpServer.address();
    if (address && typeof address !== "string")
        console.log(`tcp listen: ${address.port}`);
});

const listenPath = process.env.MalterlibWebTestWebHttpSocket || (process.cwd() + "/http.sock");
const listenPathTLS = process.env.MalterlibWebTestWebHttpsSocket || (process.cwd() + "/https.sock");

app.on("error", (error) => {
    console.log(`ERROR: ${error}`);
});

let httpServer = http.createServer(app);
httpServer.listen(listenPath, () => {
    console.log(`http listen: ${listenPath}`);
});

let httpsServer;

if (process.env.MalterlibWebTestWebCert) {
	const options = {
	  key: fs.readFileSync(process.env.MalterlibWebTestWebKey),
	  cert: fs.readFileSync(process.env.MalterlibWebTestWebCert)
	};

	httpsServer = https.createServer(options, app);
	httpsServer.listen(listenPathTLS, () => {
		console.log(`https listen: ${listenPathTLS}`);
	});
}

process.on('SIGINT', () => {
  process.exit(0);
});

process.on('SIGTERM', () => {
  process.exit(0);
});
