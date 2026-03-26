
/*
 * Copyright 2010-2026, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
/**
 * To build this example see CMakeLists.txt or Makefile in current directory.
 * Prerequisites to run this test:
 * 1. Run Tarantool instance on localhost and set listening port 3301;
 * 2. Create space with id = 512 without format (primary key must index the first field)
 * 3. Grant read-write privileges for guest (or simply box.schema.user.grant('guest', 'super'))
 * 4. Compile and run ./Schemaless
 */

#include "../src/Buffer/Buffer.hpp"
#include "../src/Client/Connector.hpp"

#include "Reader.hpp"

const char *address = "127.0.0.1";
int port = 3301;
int WAIT_TIMEOUT = 1000; // milliseconds

using Buf_t = tnt::Buffer<16 * 1024>;
#include "../src/Client/LibevNetProvider.hpp"
using Net_t = LibevNetProvider<Buf_t, DefaultStream>;

/** A helper that checks that there is no error and returns amount of tuples in the response. */
template <class BUFFER, class Data = std::vector<UserTuple>>
size_t
responseTupleCount(Response<BUFFER> &response, Data data = std::vector<UserTuple>())
{
	if (response.body.error_stack != std::nullopt) {
		std::cerr << "The response has an error" << std::endl;
		exit(-1);
	}
	if (!response.body.data->decode(data)) {
		std::cerr << "Failed to decode data" << std::endl;
		exit(-1);
	}
	return std::size(data);
}

/** A helper that checks that there is no error in the response. */
template <class BUFFER, class Data = std::vector<UserTuple>>
void
responseCheckOk(Response<BUFFER> &response)
{
	if (response.body.error_stack != std::nullopt) {
		std::cerr << "The response has an error" << std::endl;
		exit(-1);
	}
}

int
main()
{
	/*
	 * Create default connector.
	 */
	Connector<Buf_t, Net_t> client;
	/*
	 * Create single connection. Constructor takes only client reference.
	 */
	Connection<Buf_t, Net_t> conn(client);
	/*
	 * Try to connect to given address:port. Current implementation is
	 * exception free, so we rely only on return codes.
	 */
	int rc = client.connect(conn,
				{.address = address, .service = std::to_string(port),
				 /*.user = ...,*/
				 /*.passwd = ...,*/
				 /* .transport = STREAM_SSL, */});
	if (rc != 0) {
		std::cerr << conn.getError().msg << std::endl;
		return -1;
	}

	/* ID of the space we are working with. */
	uint32_t space_id = 512;

	/*
	 * Stream ID for the writer transaction. Note that ID = 0 makes no sense
	 * because stream won't be used then.
	 */
	stream_id_t writer_sid = 1;

	/*
	 * Request to begin writing transaction.
	 * We create a simple transaction here with default isolation level and no timeout.
	 * More about these options can be found in documentation.
	 *
	 * NB: do not forget to use engine with MVCC in Tarantool!
	 */
	rid_t writer_begin = conn.stream[writer_sid].begin();
	client.wait(conn, writer_begin);
	assert(conn.futureIsReady(writer_begin));
	auto response = conn.getResponse(writer_begin);
	responseCheckOk(response);

	/* Write a tuple in the transaction. */
	std::tuple data = std::make_tuple(11, "111", 1.01);
	rid_t insert = conn.stream[writer_sid].space[space_id].insert(data);
	client.wait(conn, insert);
	assert(conn.futureIsReady(insert));
	response = conn.getResponse(insert);
	/* Insert returns the newly inserted tuple. */
	assert(responseTupleCount(response) == 1);

	/* Select a tuple without in another transaction - the write above is not visible. */
	rid_t select = conn.space[space_id].select(std::make_tuple(11), 0, 100, 0, IteratorType::EQ);
	client.wait(conn, select);
	assert(conn.futureIsReady(select));
	response = conn.getResponse(select);
	/* That write wasn't committed so we must read no tuples here. */
	assert(responseTupleCount(response) == 0);

	/* Commit the writer. */
	rid_t writer_commit = conn.stream[writer_sid].commit();
	client.wait(conn, writer_commit);
	assert(conn.futureIsReady(writer_commit));
	response = conn.getResponse(writer_commit);
	responseCheckOk(response);

	/* That write was committed so we must read one tuple in another transaction. */
	select = conn.space[space_id].select(std::make_tuple(11), 0, 100, 0, IteratorType::EQ);
	client.wait(conn, select);
	assert(conn.futureIsReady(select));
	response = conn.getResponse(select);
	assert(responseTupleCount(response) == 1);

	/* Finally, user is responsible for closing connections. */
	client.close(conn);
	return 0;
}
