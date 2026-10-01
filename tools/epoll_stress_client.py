#!/usr/bin/env python3
"""Concurrent integrity and throughput tester for the xv6 epoll echo server."""

import argparse
import asyncio
import sys
import time


MAX_SERVER_CLIENTS = 15
DEFAULT_CHUNK_SIZE = 1024


class EchoMismatch(Exception):
    pass


def make_payload(client_id: int, offset: int, size: int) -> bytes:
    return bytes(
        (((offset + index) * 131 + client_id * 17 +
          ((offset + index) >> 8)) & 0xFF)
        for index in range(size)
    )


async def send_payload(
    writer: asyncio.StreamWriter,
    client_id: int,
    total_bytes: int,
    chunk_size: int,
) -> None:
    for offset in range(0, total_bytes, chunk_size):
        payload = make_payload(
            client_id, offset, min(chunk_size, total_bytes - offset)
        )
        writer.write(payload)
        await writer.drain()
    writer.write_eof()
    await writer.drain()


async def receive_echo(
    reader: asyncio.StreamReader,
    client_id: int,
    total_bytes: int,
    chunk_size: int,
) -> None:
    for offset in range(0, total_bytes, chunk_size):
        size = min(chunk_size, total_bytes - offset)
        echoed = await reader.readexactly(size)
        expected = make_payload(client_id, offset, size)
        if echoed != expected:
            mismatch = next(
                index
                for index, (actual, wanted) in enumerate(zip(echoed, expected))
                if actual != wanted
            )
            raise EchoMismatch(
                f"client {client_id}: echo differs at byte {offset + mismatch}"
            )

    extra = await reader.read(1)
    if extra:
        raise EchoMismatch(f"client {client_id}: server echoed extra data")


async def run_client(
    host: str,
    port: int,
    client_id: int,
    total_bytes: int,
    chunk_size: int,
) -> None:
    reader, writer = await asyncio.open_connection(host, port)
    send_task = asyncio.create_task(
        send_payload(writer, client_id, total_bytes, chunk_size)
    )
    receive_task = asyncio.create_task(
        receive_echo(reader, client_id, total_bytes, chunk_size)
    )
    tasks = (send_task, receive_task)
    try:
        done, pending = await asyncio.wait(
            tasks, return_when=asyncio.FIRST_EXCEPTION
        )
        for task in done:
            task.result()
        if pending:
            await asyncio.gather(*pending)
    finally:
        for task in tasks:
            if not task.done():
                task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        writer.close()
        await writer.wait_closed()


async def run_round(args: argparse.Namespace) -> float:
    started = time.perf_counter()
    await asyncio.wait_for(
        asyncio.gather(
            *(
                run_client(
                    args.host,
                    args.port,
                    client_id,
                    args.bytes_per_client,
                    args.chunk_size,
                )
                for client_id in range(args.clients)
            )
        ),
        timeout=args.timeout,
    )
    return time.perf_counter() - started


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Stress-test the xv6 epollserver TCP echo service."
    )
    parser.add_argument("host", help="IP address or hostname of the xv6 board")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument(
        "--clients",
        type=int,
        default=8,
        help=f"concurrent clients (1-{MAX_SERVER_CLIENTS}; default: 8)",
    )
    parser.add_argument(
        "--bytes",
        dest="bytes_per_client",
        type=int,
        default=64 * 1024,
        help="bytes sent by each client in each round (default: 65536)",
    )
    parser.add_argument("--chunk-size", type=int, default=DEFAULT_CHUNK_SIZE)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=60.0)
    args = parser.parse_args()

    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    if not 1 <= args.clients <= MAX_SERVER_CLIENTS:
        parser.error(f"--clients must be between 1 and {MAX_SERVER_CLIENTS}")
    if args.bytes_per_client <= 0:
        parser.error("--bytes must be greater than zero")
    if args.chunk_size <= 0:
        parser.error("--chunk-size must be greater than zero")
    if args.rounds <= 0:
        parser.error("--rounds must be greater than zero")
    if args.timeout <= 0:
        parser.error("--timeout must be greater than zero")
    return args


async def run(args: argparse.Namespace) -> None:
    durations = []
    total_bytes = args.clients * args.bytes_per_client
    print(
        f"Testing {args.host}:{args.port}: {args.clients} clients, "
        f"{args.bytes_per_client} bytes/client, {args.rounds} rounds"
    )

    for round_number in range(1, args.rounds + 1):
        duration = await run_round(args)
        durations.append(duration)
        print(
            f"round {round_number}/{args.rounds}: "
            f"{total_bytes} bytes echoed in {duration:.3f}s"
        )

    elapsed = sum(durations)
    transferred = total_bytes * args.rounds
    rate = transferred / elapsed if elapsed else 0.0
    print(
        f"PASS: {transferred} bytes verified across "
        f"{args.clients * args.rounds} connections in {elapsed:.3f}s "
        f"({rate:.0f} bytes/s)"
    )


def main() -> int:
    args = parse_args()
    try:
        asyncio.run(run(args))
    except (OSError, asyncio.TimeoutError, asyncio.IncompleteReadError,
            EchoMismatch) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
