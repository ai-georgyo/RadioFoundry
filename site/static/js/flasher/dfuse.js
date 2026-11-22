// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The flasher's backend for radios installed through their processor's own ROM bootloader by plain DfuSe (board
// family "dfuse", dfu.js runPlain): no key, no stock file and no codec step. Its install is
//
//   radio, build   the shared steps (app.js)
//   write          the plan is made and held to the route's limits first (dfu.js checkPlain). Then: enter the loader
//                  with the board's keys, or, on a radio that runs RadioFoundry and whose firmware can, by a restart
//                  the page asks for over the debug link (restartIntoLoader), and connect; the device is checked
//                  (dfu.js romDevice) and asked whether its
//                  program memory is read protected. Not protected: Install erases the image's sectors, writes them
//                  and leaves at the application base. Protected (a radio on its original firmware): nothing can be
//                  written until a Read Unprotect, which erases the whole program memory, the original firmware and
//                  its bootloader with it. That is a step of its own that the user confirms; the loader then leaves
//                  the bus, the page waits out the erase (unprotectWait), the user enters the loader again and
//                  connects again (a new click: the browser asks for the device afresh), and the probe decides once
//                  more
//   done           the shared check over the debug link (app.js doneView)
//
// What stays reachable: the loader is in the chip's system memory, which nothing here can write (no alternate
// setting is ever selected, so the interface stays on the internal flash; the option bytes are never touched), so a
// radio can always be installed again. What the page cannot do is put the original firmware back: that is the maker's
// own procedure, and the recovery says so in words (recovery.stock "maker"). A backup of such a radio holds no codec,
// so a restore puts back its channels and settings only (restores), and the firmware comes back with Install. The
// original firmware's codeplug is in no layout the page converts, so the install has no channel step (stockChannels).

import { h, t, notice, button, progressBar, logBox } from "./dom.js";
import { Dfu, DfuError, WrongDeviceError, romDevice, checkPlain } from "./dfu.js";
import { RESTART_WHY } from "./link.js";
import { sleep } from "./bytes.js";
import { makePlan, KEEP } from "./plan.js";
import { segments } from "./manifest.js";
import { register } from "./backends.js";
import { errorNotice } from "../report.js";
import * as diag from "../diag.js";

export const ID = "dfuse";
// The wait after a Read Unprotect, rfoundry's (cli.py unprotect_wait): the chip erases its whole program flash, then
// leaves USB (AN3156 5.4, figure 10). How long that erase takes at most is the chip's own figure, the board's
// mass_erase_max_s in data/flash-boards.json (the registry's, from the board's platform.toml). The least wait is
// never shorter than UNPROTECT_MIN_S, nor the longest one than UNPROTECT_WAIT_S
export const UNPROTECT_MIN_S = 60, UNPROTECT_WAIT_S = 120;

// {least, most, bound}, in seconds from a Read Unprotect. least: the page says the radio may be switched off no
// sooner, twice the chip's longest mass erase (bound) and never under UNPROTECT_MIN_S: a chip with two flash banks may
// erase them one after the other, and the RAM erase and option byte change that follow have no time given. most: how
// long the page waits for the loader to leave USB, twice the least wait and never under UNPROTECT_WAIT_S: one still on
// USB then has long finished the erase, or never started it. A board whose data gives no erase time is refused: the
// page could not say when the radio may be switched off
export function unprotectWait(board) {
	const bound = board.mass_erase_max_s;
	if (!(bound > 0)) throw new DfuError(`the ${board.name || "radio"}'s data gives no mass erase time for its processor: the page could not say when the radio may be switched off after its read protection is removed. Refusing`);
	const least = Math.max(UNPROTECT_MIN_S, 2 * bound);
	return { least, most: Math.max(UNPROTECT_WAIT_S, 2 * least), bound };
}

// How long the page watches a radio that took the restart into its loader: it resets 250 ms after its reply
// (docs/spec/debuglink.md 3.11), and its serial port goes with it. One that still answers after RESTART_CHECK_MS
// stayed up: a check failed again at the reset
export const RESTART_CHECK_MS = 3000, RESTART_TICK_MS = 500;

// Ask a radio that runs RadioFoundry to restart into its processor's loader, over the debug link, as rfoundry does
// (cli.py restart_into_bootloader). Only the radio the page was talking to is asked: the board it reports must be
// this board's, and its chip UID the one read when it connected (uid; null when it gave none). {kind, why}:
//   "ok"       it took the restart and its port went: the loader is the next USB device to pick
//   "not-now"  the board's own rule refuses it just now (reason 5): nothing changed, the board's hint says what to do
//   "stayed"   it answered OK and still runs after waitMs
//   "other"    not the radio the page meant: nothing was asked
//   "no-chip"  it gave no chip info, so there is no token to send: nothing was asked
//   a refusal's kind (link.js restartOutcome), with why, its words
// Throws when the radio doesn't answer
export async function restartIntoLoader(link, board, { uid = null, waitMs = RESTART_CHECK_MS, tickMs = RESTART_TICK_MS } = {}) {
	const v = await link.version();
	const chip = await link.chip();
	if (!board.debuglink_names.includes(v.board) || (chip && uid && chip.uid !== uid)) return { kind: "other", why: "another radio answers on this connection" };
	if (!chip) return { kind: "no-chip", why: RESTART_WHY["no-chip"] };
	const kind = await link.restartIntoBootloader(chip);
	if (kind === "not-now") return { kind };
	if (kind !== "ok") return { kind, why: RESTART_WHY[kind] || "it answered the request with an error" };
	for (let waited = 0; waited < waitMs; waited += tickMs) {
		await sleep(tickMs);
		try { await link.version(); } catch (e) { return { kind }; }
	}
	return { kind: "stayed" };
}

let ctx = null;		// from app.js: {S, fb, next, render, reset, helpLink, manualKitLine, flashStarted, flashFinished, flashMark, errorText,
			// connectBootloader, dropSerial, withPicture, driverHelp, driverWorked, ensureFirmware, notePlan, doneView,
			// backupView, restoreView}; a test may add eraseWait: {seconds, maxSeconds, tickMs} and restartWait: {waitMs, tickMs}
const S = () => ctx.S;

export function init(context) {
	ctx = context;
}

// Radios the page installs this way: the backend's, with the loader's USB IDs in the board's data
export const isDfuse = (radio, fb) => radio?.flash?.backend === ID && !!fb?.boards?.[radio.flash.board]?.dfu_ids;

// The install's plan: the image alone, one segment from the application base. Keep mode with no codec windows writes
// the sectors the image covers and touches no other, the vector table last (checkPlain), as rfoundry's plan. The
// route's limits are checked here, when the step is drawn, and not only by the write: a plan the write would refuse
// (an own image that reaches past the board's application flash) must be refused before the user is offered the erase
// of everything
export function installPlan(board, fw) {
	const plan = makePlan(board, segments(fw.manifest, fw.image), KEEP, [], board.codec.windows, { vectorLast: true });
	checkPlain(plan);
	return plan;
}

// What the loader said when asked: "ready" (not protected: the write may run) or "protected". Throws what stops the
// install before anything is written: a device that is not this radio's loader, or a probe without a clear answer
export async function checkLoader(transport, board, { log = () => {}, wait } = {}) {
	const info = await transport.fingerprint?.();
	romDevice(board, info);
	log(`loader: ${info?.string1 || "?"} / ${info?.product || "?"} / serial ${info?.serial || "?"} / ${info?.interfaces?.[0] || ""}`);
	const dfu = new Dfu(transport, wait ? { log, wait } : { log });
	return { dfu, stage: (await dfu.probeProtected(board)) ? "protected" : "ready" };
}

// The write step. One panel in four states: enter (the loader's keys, or the restart into it, then Connect),
// protected (the confirmed Read Unprotect), erasing (the wait), ready (Install)
export function writeView() {
	const state = S();
	if (!state.firmware) {	// an update goes straight from its first step to this one
		const out = h("p", { class: "muted" }, t("flash.loading"));
		ctx.ensureFirmware().then(ctx.render, (e) => out.replaceChildren(errorNotice(ctx.errorText(e), { retry: ctx.render })));
		return out;
	}
	const fw = state.firmware, board = state.board;
	const what = { task: "install", source: fw.origin || "", radio: board.id, firmware: String(fw.manifest.version || ""),
		variant: fw.variant || fw.manifest.variant || (fw.manifest.dev_build ? "dev" : ""), features: fw.manifest.features || "", build: fw.build || null };
	const box = h("section", { class: "panel" });
	const log = logBox();
	const prog = progressBar();
	const status = h("div");
	let plan, waits;
	try {
		plan = installPlan(board, fw);
		waits = unprotectWait(board);	// here too: no Read Unprotect is offered without knowing how long it is waited out
		ctx.notePlan(plan);
	} catch (e) {
		state.forward(null);
		box.append(h("h2", {}, t("flash.write.title")), errorNotice(ctx.errorText(e)));
		return box;
	}
	// unprotected: a Read Unprotect ran in this visit, so the next probe says whether it worked. sentAt: when it was
	// sent; left: the loader has left USB since (the transport's watchLeave), null where nothing can tell. probed: the
	// loader has answered the probe since that Read Unprotect, so what the wait left open is settled
	let stage = "enter", transport = null, dfu = null, unprotected = false, timer = null, logged = false, sentAt = 0, left = null, unwatch = null, waited = 0,
		probed = false;
	const stopWatch = () => {
		try { unwatch?.(); } catch (e) { /* gone */ }
		unwatch = null;
	};
	const winHelp = h("div");	// the driver steps as they are before a connect; a failed one shows them open instead
	const drop = async () => {
		try { await transport?.close?.(); } catch (e) { /* gone */ }
		transport = null;
		dfu = null;
	};
	const failed = (e, key = null) => {
		const raw = ctx.errorText(e);	// logged once, with the step it happened in
		const text = e instanceof WrongDeviceError ? t("flash.dfuse.wrong_device", { what: e.what }) : raw;
		if (stage === "enter") winHelp.replaceChildren();
		status.replaceChildren(...h("div", {}, errorNotice(key ? t(key, { error: text }) : text),
			stage === "enter" ? ctx.driverHelp({ failed: true }) : null,
			e?.name === "NotFoundError" ? null : ctx.helpLink(), e?.name === "NotFoundError" ? null : ctx.manualKitLine()).childNodes);
	};

	const connect = (primary = true) => button(t("flash.write.connect"), async (ev) => {
		const btn = ev?.currentTarget;
		if (btn) btn.disabled = true;
		status.replaceChildren(h("p", { class: "muted", role: "status" }, t("flash.dfuse.probing")));
		try {
			transport = await ctx.connectBootloader();
			const found = await checkLoader(transport, board, { log: log.add });
			dfu = found.dfu;
			probed = true;
			ctx.driverWorked();
			diag.note("loader", { protected: found.stage === "protected", after_unprotect: unprotected });
			status.replaceChildren();
			restartNote.replaceChildren();
			stage = found.stage;
			draw();
		} catch (e) {
			await drop();
			failed(e);
			if (btn) btn.disabled = false;
		}
	}, { primary });

	// The restart from the running firmware, instead of the loader's keys. Offered while the page holds the serial
	// port of a radio that runs RadioFoundry and the board's firmware can do it (radios.json flash.bootloader_restart).
	// What the radio said stays under the offer (restartNote) until the loader is connected. A refusal is nobody's
	// mistake: it is said with the board's own hint (flash.bootloader_restart_hint), and the keys' steps stay below
	const restartNote = h("div");
	const canRestart = () => !!state.radio.flash.bootloader_restart && state.now?.kind === "rfoundry" && !!state.link && !state.link.s?.lost
		&& !(state.fakeRadio && state.fakeRadio.mode !== "normal");
	const restart = () => button(t("flash.dfuse.restart_go"), async (ev) => {
		const btn = ev?.currentTarget;
		if (btn) btn.disabled = true;
		restartNote.replaceChildren(h("p", { class: "muted", role: "status" }, t("flash.dfuse.restarting")));
		status.replaceChildren();
		let r;
		try {
			r = await restartIntoLoader(state.link, board, { uid: state.now?.uid || null, ...(ctx.restartWait || (state.fakeRadio ? { waitMs: 300, tickMs: 50 } : {})) });
		} catch (e) {
			r = { kind: "silent", why: ctx.errorText(e) };
		}
		diag.note("restart into the loader", { kind: r.kind });
		if (r.kind === "ok") {
			await ctx.dropSerial();		// the port went with the restart
			restartNote.replaceChildren(notice(t("flash.dfuse.restart_done"), "ok"));
		} else if (r.kind === "not-now") {
			const hint = state.radio.flash.bootloader_restart_hint;
			restartNote.replaceChildren(h("div", { class: "notice action" }, h("p", {}, t("flash.dfuse.restart_refused")),
				hint ? h("p", {}, h("strong", {}, hint)) : null,
				state.fakeRadio ? h("p", { class: "small" }, t("flash.sim.restart_note")) : null));
		} else if (r.kind === "stayed") restartNote.replaceChildren(notice(t("flash.dfuse.restart_stayed"), "bad"));
		// the one other refusal a user can end by themselves; the offer stays for the next try
		else if (r.kind === "transmitting") restartNote.replaceChildren(notice(t("flash.dfuse.restart_transmitting"), "action"));
		else restartNote.replaceChildren(notice(t(r.kind === "silent" ? "flash.dfuse.restart_no_answer" : "flash.dfuse.restart_failed", { error: r.why }), "bad"));
		draw();
	}, { primary: true });

	const unprotect = (agree) => button(t("flash.dfuse.unprotect_go"), async (ev) => {
		const btn = ev?.currentTarget;
		if (!agree.checked) return;
		if (btn) btn.disabled = true;
		try {
			// watched before the command: the loader may leave the bus at once (AN3156 5.4)
			stopWatch();
			left = transport.watchLeave ? false : null;
			unwatch = transport.watchLeave?.(() => { left = true; diag.note("loader left USB after the read unprotect"); }) ?? null;
			sentAt = Date.now();
			await dfu.readUnprotect(board);
			diag.note("read unprotect sent");
			unprotected = true;
			probed = false;
			await drop();
			status.replaceChildren();
			stage = "erasing";
			draw();
		} catch (e) {
			stopWatch();
			failed(e, "flash.dfuse.unprotect_failed");
			if (btn) btn.disabled = false;
		}
	}, { danger: true, disabled: true });

	const write = () => button(t("flash.write.go"), async (ev) => {
		const btn = ev?.currentTarget;
		if (btn) btn.disabled = true;
		logged = true;
		box.append(prog.el, log.el);
		let started = null;
		try {
			await ctx.dropSerial();
			started = ctx.flashStarted(what);
			await dfu.run(plan, null, (d, total) => {
				if (d > 0) ctx.flashMark(started, "first_byte");
				prog.set(d, total, t("flash.write.writing"));
			});
			ctx.flashMark(started, "last_byte");
			ctx.flashFinished(started);
			log.add(t("flash.write.finished"));
			await drop();
			ctx.next();
		} catch (e) {
			ctx.flashFinished(started, e);
			await drop();
			stage = "enter";
			draw();
			failed(e, "flash.write.failed");
		}
	}, { primary: true });

	// The erase, as rfoundry waits it out (unprotectWait above): the bar counts the least wait, then the page goes on
	// once the loader has left USB, or at the longest wait. Nothing says the radio may be switched off before that. In
	// practice mode a few seconds stand in (ctx.eraseWait: the tests' own counts and tick)
	const wait = () => {
		const least = ctx.eraseWait?.seconds ?? (state.fakeRadio ? 3 : waits.least);
		const most = ctx.eraseWait?.maxSeconds ?? (state.fakeRadio ? 6 : waits.most), tickMs = ctx.eraseWait?.tickMs ?? 1000;
		const tick = () => {
			const s = Math.min(most, Math.floor((Date.now() - sentAt) / tickMs));
			if (!box.isConnected && s > 0) { stopWatch(); return clearInterval(timer); }	// the step was left: the radio finishes by itself
			if (s < least) prog.set(s, least, t("flash.dfuse.erasing_bar", { s: least - s }));
			else prog.set(s, most, t("flash.dfuse.leaving_bar", { s: most - s }));
			if (s < least || (s < most && !left)) return;
			clearInterval(timer);
			timer = null;
			stopWatch();
			diag.note("erase wait over", { left, s });
			waited = s;
			stage = "enter";
			draw();
		};
		clearInterval(timer);
		timer = setInterval(tick, tickMs);
		tick();
	};

	// after a Read Unprotect: "the erase is over" only when the loader was seen to leave USB. Only until the loader
	// has answered the probe again: after that (a write that failed, say) these are the plain steps into the loader
	const reenterText = () => (left ? t("flash.dfuse.reenter") : t("flash.dfuse.reenter_unsure", { s: waited, max: waits.bound }));
	const enterSteps = (offer) => [
		h("h3", {}, t(unprotected && !probed ? "flash.dfuse.step_reenter" : "flash.write.bootloader_title")),
		unprotected && !probed ? h("p", {}, reenterText()) : null,
		// what happens next is said before the click: the loader is another USB device, which the browser asks for
		offer ? h("div", { class: "notice info" }, h("p", {}, t("flash.dfuse.restart_offer")), h("p", {}, restart()),
			h("p", { class: "small" }, t("flash.dfuse.restart_next"))) : null,
		restartNote,
		offer ? h("p", {}, t("flash.dfuse.restart_by_hand")) : null,
		ctx.withPicture(h("ol", { class: "steps" }, state.radio.flash.bootloader.map((s) => h("li", {}, s)), h("li", {}, t("flash.write.then_connect")))),
		state.fakeRadio ? h("p", {}, button(t("flash.sim.enter_bootloader"), () => { state.fakeRadio.enterBootloader(); log.add("(simulated) radio in update mode"); }, { quiet: true })) : null,
		winHelp];
	// the plan in rfoundry's words (describe() says "no codec" for a board without a codec route, as plan.py does)
	const details = () => h("details", { class: "small" }, h("summary", {}, t("flash.write.plan", { erase: plan.erase.length, blocks: plan.totalBlocks })),
		h("pre", { class: "log" }, plan.describe().join("\n")));

	function draw() {
		let body, forward;
		if (stage === "protected") {
			const agree = h("input", { type: "checkbox", id: "dfuse-agree" });
			const go = unprotect(agree);
			agree.addEventListener("change", () => { go.disabled = !agree.checked; });
			body = [h("h3", {}, t("flash.dfuse.step_unprotect")),
				unprotected ? notice(t("flash.dfuse.still_protected"), "bad") : null,
				h("div", { class: "notice bad" }, h("p", {}, h("strong", {}, t("flash.dfuse.protected_title"))),
					h("p", {}, t("flash.dfuse.protected_text")), h("p", {}, t("flash.dfuse.erase_warning"))),
				h("div", { class: "notice action" }, h("p", {}, t("flash.dfuse.precondition"))),
				state.radio.flash.notice ? h("p", { class: "small muted" }, state.radio.flash.notice) : null,
				h("label", { class: "toggle", for: "dfuse-agree" }, agree, h("span", {}, h("b", {}, t("flash.dfuse.confirm"))), h("span"))];
			forward = go;
		} else if (stage === "erasing") {
			body = [h("h3", {}, t("flash.dfuse.step_erasing")), notice(t("flash.dfuse.erasing", { s: waits.least }), "action"), prog.el];
			forward = null;
		} else if (stage === "ready") {
			body = [notice(`${t("flash.write.found")} ${t(unprotected ? "flash.dfuse.unprotected_ok" : "flash.dfuse.not_protected")}`, "ok")];
			forward = write();
		} else {
			winHelp.replaceChildren(...[ctx.driverHelp()].filter(Boolean));
			const offer = canRestart();
			body = enterSteps(offer);
			forward = connect(!offer);	// while the restart is offered, it is the step's first button
		}
		state.forward(forward);
		// through h(): the DOM's own replaceChildren writes a null as the text "null". The log stays once a write ran
		box.replaceChildren(...h("div", {}, h("h2", {}, t("flash.write.title")), ...body, status, details(), logged ? log.el : null).childNodes);
		if (stage === "erasing") wait();
	}
	draw();
	return box;
}

// Stock firmware by the maker's own procedure, said in words: the help task's "Restore Original Firmware" on a radio
// of this backend (app.js recoverView draws it for recovery.stock "maker")
export function makerRecovery() {
	const state = S();
	return h("div", {}, h("p", {}, t("flash.dfuse.maker_text")), h("p", {}, t("flash.dfuse.maker_loader")),
		state.radio?.flash.notice ? h("p", { class: "small muted" }, state.radio.flash.notice) : null);
}

// The backend (backends.js). An update is the install without the radio step: a radio that runs RadioFoundry enters
// the loader the same way, or by the restart the write step offers, and is not read protected. recovering: the help
// task's reinstall, the radio known
const INSTALL = ["radio", "build", "write", "done"], RECOVER = ["build", "write", "done"], UPDATE = ["update", "write", "done"];
register({
	id: ID,
	available: (radio, fb, task) => (task === "backup" ? radio.backup?.rfoundry === "full" : isDfuse(radio, fb)),
	steps: (task, { recovering } = {}) => (task === "install" ? (recovering ? RECOVER : INSTALL) : task === "update" ? UPDATE : null),
	views: { write: writeView, done: () => ctx.doneView(), backup: () => ctx.backupView(), restore: () => ctx.restoreView() },
	recovery: { first: "updatemode", stock: "maker", reach: null, stockText: "flash.dfuse.fix_stock_text", calm: "flash.dfuse.calm_text",
		maker: makerRecovery },
	// a backup's program memory holds RadioFoundry and nothing else of the radio's: Install brings that back
	restores: { targets: ["codeplug"], note: "flash.dfuse.restore_note" },
	// the build step's line for a radio still in development: there is no stock file to keep
	untested: "flash.dfuse.untested",
	// no channel step after the install: nothing is known of where the original firmware keeps its codeplug, so the
	// page reads none, converts none, and never prepares the memory chip for one it would write without a backup
	stockChannels: false,
});
