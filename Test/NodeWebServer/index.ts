import express from "express";

import https from "https";
import http from "http";
let fs = require("fs");
//let process = require("process");

const app = express();

app.get("/", (req, res) => {
    res.send("Root Reply");
})

const listenPath = process.cwd() + "/http.sock";
const listenPathTLS = process.cwd() + "/https.sock";

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
