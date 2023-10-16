/*
 * Copyright 2010-2020, Tarantool AUTHORS, please see AUTHORS file.
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
#include "Utils/Helpers.hpp"
#include "Utils/TupleReader.hpp"
#include "Utils/System.hpp"

#include "../src/Client/LibevNetProvider.hpp"
#include "../src/Client/Connector.hpp"

const char *localhost = "127.0.0.1";
int port = 3301;
int dummy_server_port = 3302;
const char *unixsocket = "./tnt.sock";
int WAIT_TIMEOUT = 1000; //milliseconds

#ifdef TNTCXX_ENABLE_SSL
constexpr bool enable_ssl = true;
constexpr StreamTransport transport = STREAM_SSL;
#else
constexpr bool enable_ssl = false;
constexpr StreamTransport transport = STREAM_PLAIN;
#endif

#ifdef __linux__
using NetProvider = EpollNetProvider<Buf_t, DefaultStream>;
#else
using NetProvider = LibevNetProvider<Buf_t, DefaultStream>;
#endif

/**
 * Kills tarantool process.
 */
void atexit_handler(void)
{

}

template <class Connector, class Connection>
static int
test_connect(Connector &client, Connection &conn, const std::string &addr,
	     unsigned port,
	     const std::string user = {}, const std::string passwd = {})
{
	std::string service = port == 0 ? std::string{} : std::to_string(port);
	return client.connect(conn, {
		.address = addr,
		.service = service,
		.transport = transport,
		.user = user,
		.passwd = passwd,
	});
}

enum ResultFormat {
	TUPLES = 0,
	MULTI_RETURN,
	SELECT_RETURN
};

template <class BUFFER, class NetProvider>
void
printResponse(Connection<BUFFER, NetProvider> &conn, Response<BUFFER> &response,
	       enum ResultFormat format = TUPLES)
{
	if (response.body.error_stack != std::nullopt) {
		Error err = (*response.body.error_stack).error;
		std::cout << "RESPONSE ERROR: msg=" << err.msg <<
			  " line=" << err.file << " file=" << err.file <<
			  " errno=" << err.saved_errno <<
			  " type=" << err.type_name <<
			  " code=" << err.errcode << std::endl;
		return;
	}
	if (response.body.data != std::nullopt) {
		Data<BUFFER>& data = *response.body.data;
		if (response.body.data->sql_data != std::nullopt) {
			if (response.body.data->sql_data->sql_info  != std::nullopt) {
				std::cout << "Row count = " << response.body.data->sql_data->sql_info->row_count << std::endl;
				std::cout << "Autoinc id count = " << response.body.data->sql_data->sql_info->autoincrement_id_count << std::endl;
			}
			if (response.body.data->sql_data->metadata != std::nullopt) {
				std::vector<ColumnMap>& maps = response.body.data->sql_data->metadata->column_maps;
				std::cout << "Metadata:\n";
				for (auto& map : maps) {
					std::cout << "Field name: "       << map.field_name       << std::endl;
					std::cout << "Field name len: "   << map.field_name_len   << std::endl;
					std::cout << "Field type: "       << map.field_type       << std::endl;
					std::cout << "Field type len: "   << map.field_type_len   << std::endl;
					std::cout << "Collation: "        << map.collation        << std::endl;
					std::cout << "Collation len: "    << map.collation_len    << std::endl;
					std::cout << "Is nullable: "      << map.is_nullable      << std::endl;
					std::cout << "Is autoincrement: " << map.is_autoincrement << std::endl;
					std::cout << "Span: "             << map.span             << std::endl;
					std::cout << "Span len: "         << map.span_len         << std::endl;
				}
			}
			if (response.body.data->sql_data->stmt_id    != std::nullopt) {
				std::cout << "statement id = " << *response.body.data->sql_data->stmt_id    << std::endl;
			}
			if (response.body.data->sql_data->bind_count != std::nullopt) {
				std::cout << "bind count = "   << *response.body.data->sql_data->bind_count << std::endl;
			}
		}
		if (data.tuples.empty()) {
			std::cout << "No tuples" << std::endl;
			return;
		}
		std::vector<UserTuple> tuples;
		switch (format) {
			case TUPLES:
				tuples = decodeUserTuple(conn.getInBuf(), data);
				break;
			case MULTI_RETURN:
				tuples = decodeMultiReturn(conn.getInBuf(), data);
				break;
			case SELECT_RETURN:
				tuples = decodeSelectReturn(conn.getInBuf(), data);
				break;
			default:
				assert(0);
		}
		for (auto const& t : tuples) {
			std::cout << t << std::endl;
		}
	}
}

template<class BUFFER, class NetProvider>
bool
compareTupleResult(std::vector<UserTuple> &tuples,
		   std::vector<UserTuple> &expected);

template <class BUFFER, class NetProvider>
void
trivial(Connector<BUFFER, NetProvider> &client)
{
	TEST_INIT(0);
	Connection<Buf_t, NetProvider> conn(client);
	TEST_CASE("Nonexistent future");
	fail_unless(!conn.futureIsReady(666));
	/* Execute request without connecting to the host. */
	TEST_CASE("No established connection");
	rid_t f = conn.ping();
	int rc = client.wait(conn, f, WAIT_TIMEOUT);
	fail_unless(rc != 0);
	/* Connect to the wrong address. */
	TEST_CASE("Bad address");
	rc = test_connect(client, conn, "asdasd", port);
	fail_unless(rc != 0);
	TEST_CASE("Unreachable address");
	rc = test_connect(client, conn, "101.101.101", port);
	fail_unless(rc != 0);
	TEST_CASE("Wrong port");
	rc = test_connect(client, conn, localhost, -666);
	fail_unless(rc != 0);
	TEST_CASE("Connect timeout");
	rc = test_connect(client, conn, "8.8.8.8", port);
	fail_unless(rc != 0);
}

/** Single connection, separate/sequence pings, no errors */
template <class BUFFER, class NetProvider>
void
single_conn_ping(Connector<BUFFER, NetProvider> &client)
{
	TEST_INIT(0);
	Connection<Buf_t, NetProvider> conn(client);
	int rc = test_connect(client, conn, localhost, port);
	fail_unless(rc == 0);
	rid_t f = conn.ping();
	fail_unless(!conn.futureIsReady(f));
	client.wait(conn, f, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(f));
	std::optional<Response<Buf_t>> response = conn.getResponse(f);
	fail_unless(response != std::nullopt);
	fail_unless(response->header.code == 0);
	f = conn.ping();
	client.wait(conn, f, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(f));
	/* Second wait() should terminate immediately. */
	client.wait(conn, f, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(f));
	response = conn.getResponse(f);
	fail_unless(response != std::nullopt);
	fail_unless(response->header.code == 0);
	/* Many requests at once. */
	std::vector<rid_t > features;
	features.push_back(conn.ping());
	features.push_back(conn.ping());
	features.push_back(conn.ping());
	client.waitAll(conn, features, WAIT_TIMEOUT);
	for (size_t i = 0; i < features.size(); ++i) {
		fail_unless(conn.futureIsReady(features[i]));
		response = conn.getResponse(features[i]);
		fail_unless(response != std::nullopt);
		fail_unless(response->header.code == 0);
		fail_unless(response->body.error_stack == std::nullopt);
	}
	features.clear();
	features.push_back(conn.ping());
	features.push_back(conn.ping());
	features.push_back(conn.ping());
	client.waitCount(conn, features.size(), WAIT_TIMEOUT);
	for (size_t i = 0; i < features.size(); ++i) {
		fail_unless(conn.futureIsReady(features[i]));
		response = conn.getResponse(features[i]);
		fail_unless(response != std::nullopt);
		fail_unless(response->header.code == 0);
		fail_unless(response->body.error_stack == std::nullopt);
	}
	client.close(conn);
}

template <class BUFFER, class NetProvider>
void
auto_close(Connector<BUFFER, NetProvider> &client)
{
	TEST_INIT(0);
	{
		TEST_CASE("Without requests");
		Connection<Buf_t, NetProvider> conn(client);
		int rc = test_connect(client, conn, localhost, port);
		fail_unless(rc == 0);
	}
	{
		TEST_CASE("With requests");
		Connection<Buf_t, NetProvider> conn(client);
		int rc = test_connect(client, conn, localhost, port);
		fail_unless(rc == 0);

		rid_t f = conn.ping();
		fail_unless(!conn.futureIsReady(f));
		client.wait(conn, f, WAIT_TIMEOUT);
		fail_unless(conn.futureIsReady(f));
		std::optional<Response<Buf_t>> response = conn.getResponse(f);
		fail_unless(response != std::nullopt);
	}
}

/** Several connection, separate/sequence pings, no errors */
template <class BUFFER, class NetProvider>
void
many_conn_ping(Connector<BUFFER, NetProvider> &client)
{
	TEST_INIT(0);
	Connection<Buf_t, NetProvider> conn1(client);
	Connection<Buf_t, NetProvider> conn2(client);
	Connection<Buf_t, NetProvider> conn3(client);
	int rc = test_connect(client, conn1, localhost, port);
	fail_unless(rc == 0);
	/* Try to connect to the same port */
	rc = test_connect(client, conn2, localhost, port);
	fail_unless(rc == 0);
	/*
	 * Try to re-connect to another address whithout closing
	 * current connection.
	 */
	//rc = test_connect(client, conn2, localhost, port + 2);
	//fail_unless(rc != 0);
	rc = test_connect(client, conn3, localhost, port);
	fail_unless(rc == 0);
	rid_t f1 = conn1.ping();
	rid_t f2 = conn2.ping();
	rid_t f3 = conn3.ping();
	std::optional<Connection<Buf_t, NetProvider>> conn_opt = client.waitAny(WAIT_TIMEOUT);
	fail_unless(conn_opt.has_value());
	fail_unless(conn1.futureIsReady(f1) || conn2.futureIsReady(f2) ||
		    conn3.futureIsReady(f3));
	client.close(conn1);
	client.close(conn2);
	client.close(conn3);
}

/** Single connection, errors in response. */
template <class BUFFER, class NetProvider>
void
single_conn_error(Connector<BUFFER, NetProvider> &client)
{
	TEST_INIT(0);
	Connection<Buf_t, NetProvider> conn(client);
	int rc = test_connect(client, conn, localhost, port);
	fail_unless(rc == 0);
	/* Fake space id. */
	uint32_t space_id = -111;
	std::tuple data = std::make_tuple(666);
	rid_t f1 = conn.space[space_id].replace(data);
	client.wait(conn, f1, WAIT_TIMEOUT);
	std::optional<Response<Buf_t>> response = conn.getResponse(f1);
	fail_unless(response != std::nullopt);
	fail_unless(response->body.error_stack != std::nullopt);
	/* Wrong tuple format: missing fields. */
	space_id = 512;
	data = std::make_tuple(666);
	f1 = conn.space[space_id].replace(data);
	client.wait(conn, f1, WAIT_TIMEOUT);
	response = conn.getResponse(f1);
	fail_unless(response != std::nullopt);
	fail_unless(response->body.error_stack != std::nullopt);
	/* Wrong tuple format: type mismatch. */
	space_id = 512;
	std::tuple another_data = std::make_tuple(666, "asd", "asd");
	f1 = conn.space[space_id].replace(another_data);
	client.wait(conn, f1, WAIT_TIMEOUT);
	response = conn.getResponse(f1);
	fail_unless(response != std::nullopt);
	fail_unless(response->body.error_stack != std::nullopt);

	client.close(conn);
}

/** Single connection, separate replaces */
template <class BUFFER, class NetProvider>
void
single_conn_replace(Connector<BUFFER, NetProvider> &client)
{
	TEST_INIT(0);
	Connection<Buf_t, NetProvider> conn(client);
	int rc = test_connect(client, conn, localhost, port);
	fail_unless(rc == 0);
	uint32_t space_id = 512;
	std::tuple data = std::make_tuple(666, "111", 1.01);
	rid_t f1 = conn.space[space_id].replace(data);
	data = std::make_tuple(777, "asd", 2.02);
	rid_t f2 = conn.space[space_id].replace(data);

	client.wait(conn, f1, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(f1));
	std::optional<Response<Buf_t>> response = conn.getResponse(f1);
	printResponse<BUFFER, NetProvider>(conn, *response);
	fail_unless(response != std::nullopt);
	fail_unless(response->body.data != std::nullopt);
	fail_unless(response->body.error_stack == std::nullopt);

	client.wait(conn, f2, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(f2));
	response = conn.getResponse(f2);
	printResponse<BUFFER, NetProvider>(conn, *response);
	fail_unless(response != std::nullopt);
	fail_unless(response->body.data != std::nullopt);
	fail_unless(response->body.error_stack == std::nullopt);

	client.close(conn);
}

/** Single connection, separate inserts */
template <class BUFFER, class NetProvider>
void
single_conn_insert(Connector<BUFFER, NetProvider> &client)
{
	TEST_INIT(0);
	Connection<Buf_t, NetProvider> conn(client);
	int rc = test_connect(client, conn, localhost, port);
	fail_unless(rc == 0);
	TEST_CASE("Successful inserts");
	uint32_t space_id = 512;
	std::tuple data = std::make_tuple(123, "insert", 3.033);
	rid_t f1 = conn.space[space_id].insert(data);
	data = std::make_tuple(321, "another_insert", 2.022);
	rid_t f2 = conn.space[space_id].insert(data);

	client.wait(conn, f1, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(f1));
	std::optional<Response<Buf_t>> response = conn.getResponse(f1);
	printResponse<BUFFER, NetProvider>(conn, *response);
	fail_unless(response != std::nullopt);
	fail_unless(response->body.data != std::nullopt);
	fail_unless(response->body.error_stack == std::nullopt);

	client.wait(conn, f2, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(f2));
	response = conn.getResponse(f2);
	fail_unless(response != std::nullopt);
	fail_unless(response->body.data != std::nullopt);
	fail_unless(response->body.error_stack == std::nullopt);

	TEST_CASE("Duplicate key during insertion");
	data = std::make_tuple(321, "another_insert", 2.022);
	rid_t f3 = conn.space[space_id].insert(data);
	client.wait(conn, f3, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(f3));
	response = conn.getResponse(f3);
	fail_unless(response != std::nullopt);
	fail_unless(response->body.error_stack != std::nullopt);
	printResponse<BUFFER, NetProvider>(conn, *response);

	client.close(conn);
}

/** Single connection, separate updates */
template <class BUFFER, class NetProvider>
void
single_conn_update(Connector<BUFFER, NetProvider> &client)
{
	TEST_INIT(0);
	Connection<Buf_t, NetProvider> conn(client);
	int rc = test_connect(client, conn, localhost, port);
	fail_unless(rc == 0);
	TEST_CASE("Successful update");
	uint32_t space_id = 512;
	std::tuple key = std::make_tuple(123);
	std::tuple op1 = std::make_tuple("=", 1, "update");
	std::tuple op2 = std::make_tuple("+", 2, 12);
	rid_t f1 = conn.space[space_id].update(key, std::make_tuple(op1, op2));
	key = std::make_tuple(321);
	std::tuple op3 = std::make_tuple(":", 1, 2, 1, "!!");
	std::tuple op4 = std::make_tuple("-", 2, 5.05);
	rid_t f2 = conn.space[space_id].update(key, std::make_tuple(op3, op4));

	client.wait(conn, f1, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(f1));
	std::optional<Response<Buf_t>> response = conn.getResponse(f1);
	fail_unless(response != std::nullopt);
	fail_unless(response->body.data != std::nullopt);

	client.wait(conn, f2, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(f2));
	response = conn.getResponse(f2);
	fail_unless(response != std::nullopt);
	fail_unless(response->body.data != std::nullopt);

	client.close(conn);
}

/** Single connection, separate deletes */
template <class BUFFER, class NetProvider>
void
single_conn_delete(Connector<BUFFER, NetProvider> &client)
{
	TEST_INIT(0);
	Connection<Buf_t, NetProvider> conn(client);
	int rc = test_connect(client, conn, localhost, port);
	fail_unless(rc == 0);
	TEST_CASE("Successful deletes");
	uint32_t space_id = 512;
	std::tuple key = std::make_tuple(123);
	rid_t f1 = conn.space[space_id].delete_(key);
	key = std::make_tuple(321);
	rid_t f2 = conn.space[space_id].index[0].delete_(key);

	client.wait(conn, f1, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(f1));
	std::optional<Response<Buf_t>> response = conn.getResponse(f1);
	printResponse<BUFFER, NetProvider>(conn, *response);
	fail_unless(response != std::nullopt);
	fail_unless(response->body.data != std::nullopt);
	fail_unless(response->body.error_stack == std::nullopt);

	client.wait(conn, f2, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(f2));
	response = conn.getResponse(f2);
	fail_unless(response != std::nullopt);
	fail_unless(response->body.data != std::nullopt);
	fail_unless(response->body.error_stack == std::nullopt);

	TEST_CASE("Delete by wrong key (empty response)");
	key = std::make_tuple(10101);
	rid_t f3 = conn.space[space_id].delete_(key);
	client.wait(conn, f3, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(f3));
	response = conn.getResponse(f3);
	fail_unless(response != std::nullopt);
	fail_unless(response->body.data != std::nullopt);
	printResponse<BUFFER, NetProvider>(conn, *response);

	client.close(conn);
}

/** Single connection, separate upserts */
template <class BUFFER, class NetProvider>
void
single_conn_upsert(Connector<BUFFER, NetProvider> &client)
{
	TEST_INIT(0);
	Connection<Buf_t, NetProvider> conn(client);
	int rc = test_connect(client, conn, localhost, port);
	fail_unless(rc == 0);
	TEST_CASE("upsert-insert");
	uint32_t space_id = 512;
	std::tuple tuple = std::make_tuple(333, "upsert-insert", 0.0);
	std::tuple op1 = std::make_tuple("=", 1, "upsert");
	rid_t f1 = conn.space[space_id].upsert(tuple, std::make_tuple(op1));

	client.wait(conn, f1, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(f1));
	std::optional<Response<Buf_t>> response = conn.getResponse(f1);
	fail_unless(response->body.data != std::nullopt);

	TEST_CASE("upsert-update");
	tuple = std::make_tuple(666, "111", 1.01);
	std::tuple op2 =  std::make_tuple("=", 1, "upsert-update");
	rid_t f2 = conn.space[space_id].upsert(tuple, std::make_tuple(op2));
	client.wait(conn, f2, WAIT_TIMEOUT);
	response = conn.getResponse(f2);
	fail_unless(response->body.data != std::nullopt);

	client.close(conn);
}

/** Single connection, select single tuple */
template <class BUFFER, class NetProvider>
void
single_conn_select(Connector<BUFFER, NetProvider> &client)
{
	TEST_INIT(0);
	Connection<Buf_t, NetProvider> conn(client);
	int rc = test_connect(client, conn, localhost, port);
	fail_unless(rc == 0);
	uint32_t space_id = 512;
	uint32_t index_id = 0;
	uint32_t limit = 1;
	uint32_t offset = 0;
	IteratorType iter = IteratorType::EQ;

	auto s = conn.space[space_id];
	rid_t f1 = s.select(std::make_tuple(666));
	rid_t f2 = s.index[index_id].select(std::make_tuple(777));
	rid_t f3 = s.select(std::make_tuple(-1), index_id, limit, offset, iter);
	rid_t f4 = s.select(std::make_tuple(), index_id, limit + 3, offset, IteratorType::ALL);

	client.wait(conn, f1, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(f1));
	std::optional<Response<Buf_t>> response = conn.getResponse(f1);
	fail_unless(response != std::nullopt);
	fail_unless(response->body.data != std::nullopt);
	fail_unless(response->body.error_stack == std::nullopt);
	printResponse<BUFFER, NetProvider>(conn, *response);

	client.wait(conn, f2, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(f2));
	response = conn.getResponse(f2);
	fail_unless(response != std::nullopt);
	fail_unless(response->body.data != std::nullopt);
	fail_unless(response->body.error_stack == std::nullopt);
	printResponse<BUFFER, NetProvider>(conn, *response);

	client.wait(conn, f3, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(f3));
	response = conn.getResponse(f3);
	fail_unless(response != std::nullopt);
	fail_unless(response->body.data != std::nullopt);
	fail_unless(response->body.error_stack == std::nullopt);
	printResponse<BUFFER, NetProvider>(conn, *response);

	client.wait(conn, f4, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(f4));
	response = conn.getResponse(f4);
	fail_unless(response != std::nullopt);
	fail_unless(response->body.data != std::nullopt);
	fail_unless(response->body.error_stack == std::nullopt);
	printResponse<BUFFER, NetProvider>(conn, *response);

	client.close(conn);
}

/** Single connection, call procedure with arguments */
template <class BUFFER, class NetProvider>
void
single_conn_call(Connector<BUFFER, NetProvider> &client)
{
	TEST_INIT(0);
	const static char *return_replace = "remote_replace";
	const static char *return_select  = "remote_select";
	const static char *return_uint    = "remote_uint";
	const static char *return_multi   = "remote_multi";
	const static char *return_nil     = "remote_nil";
	const static char *return_map     = "remote_map";

	Connection<Buf_t, NetProvider> conn(client);
	int rc = test_connect(client, conn, localhost, port);
	fail_unless(rc == 0);

	TEST_CASE("call remote_replace");
	rid_t f1 = conn.call(return_replace, std::make_tuple(5, "value_from_test", 5.55));
	rid_t f2 = conn.call(return_replace, std::make_tuple(6, "value_from_test2", 3.33));

	client.wait(conn, f1, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(f1));
	std::optional<Response<Buf_t>> response = conn.getResponse(f1);
	fail_unless(response != std::nullopt);
	printResponse<BUFFER, NetProvider>(conn, *response);
	fail_unless(response->body.data != std::nullopt);
	fail_unless(response->body.error_stack == std::nullopt);

	client.wait(conn, f2, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(f2));
	response = conn.getResponse(f2);
	fail_unless(response != std::nullopt);
	printResponse<BUFFER, NetProvider>(conn, *response);
	fail_unless(response->body.data != std::nullopt);
	fail_unless(response->body.error_stack == std::nullopt);

	TEST_CASE("call remote_uint");
	rid_t f4 = conn.call(return_uint, std::make_tuple());
	client.wait(conn, f4, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(f4));
	response = conn.getResponse(f4);
	printResponse<BUFFER, NetProvider>(conn, *response, MULTI_RETURN);

	TEST_CASE("call remote_multi");
	rid_t f5 = conn.call(return_multi, std::make_tuple());
	client.wait(conn, f5, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(f5));
	response = conn.getResponse(f5);
	printResponse<BUFFER, NetProvider>(conn, *response, MULTI_RETURN);

	TEST_CASE("call remote_select");
	rid_t f6 = conn.call(return_select, std::make_tuple());
	client.wait(conn, f6, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(f6));
	response = conn.getResponse(f6);
	printResponse<BUFFER, NetProvider>(conn, *response, SELECT_RETURN);

	/*
	 * Also test that errors during call are handled properly:
	 * call non-existent function and pass wrong number of arguments.
	 */
	TEST_CASE("call wrong function");
	rid_t f7 = conn.call("wrong_name", std::make_tuple(7, "aaa", 0.0));
	client.wait(conn, f7, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(f7));
	response = conn.getResponse(f7);
	fail_unless(response != std::nullopt);
	fail_unless(response->body.error_stack != std::nullopt);
	printResponse<BUFFER, NetProvider>(conn, *response);

	TEST_CASE("call function with wrong number of arguments");
	rid_t f8 = conn.call(return_replace, std::make_tuple(7));
	client.wait(conn, f8, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(f8));
	response = conn.getResponse(f8);
	fail_unless(response != std::nullopt);
	fail_unless(response->body.error_stack != std::nullopt);
	printResponse<BUFFER, NetProvider>(conn, *response);

	TEST_CASE("call remote_nil");
	rid_t f9 = conn.call(return_nil, std::make_tuple());
	client.wait(conn, f9, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(f9));
	response = conn.getResponse(f9);
	printResponse<BUFFER, NetProvider>(conn, *response, MULTI_RETURN);

	TEST_CASE("call remote_map");
	rid_t f10 = conn.call(return_map, std::make_tuple());
	client.wait(conn, f10, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(f10));
	response = conn.getResponse(f10);
	printResponse<BUFFER, NetProvider>(conn, *response, MULTI_RETURN);

	client.close(conn);
}

class Stmt {
public:
	template<class BUFFER, class NetProvider>
	static std::string&
	process(Connector<BUFFER, NetProvider> &client,
		Connection<Buf_t, NetProvider> &conn,
		std::string &stmt)
	{
		(void)client;
		(void)conn;
		return stmt;
	}
};

class StmtPrepare {
public:
	template<class BUFFER, class NetProvider>
	static unsigned int
	process(Connector<BUFFER, NetProvider> &client,
		Connection<Buf_t, NetProvider> &conn,
		std::string &stmt)
	{
		rid_t future = conn.prepare(stmt);
	
		client.wait(conn, future, WAIT_TIMEOUT);
		fail_unless(conn.futureIsReady(future));
		std::optional<Response<Buf_t>> response = conn.getResponse(future);
		fail_unless(response != std::nullopt);
		fail_if(response->body.error_stack != std::nullopt);
		fail_unless(response->body.data != std::nullopt);
		fail_unless(response->body.data->sql_data->stmt_id != std::nullopt);
		fail_unless(response->body.data->sql_data->bind_count != std::nullopt);
		return response->body.data->sql_data->stmt_id.value();
	}
};

/**
 * Compares two given SqlData objects. The first one is optional, since it is
 * stored as an optional in Data. The second one is raw SqlData to make tests
 * more convenient.
 */
void
check_sql_data(const std::optional<SqlData> &got, const SqlData &expected)
{
	/* Metadata. */
	fail_unless(got->metadata.has_value() == expected.metadata.has_value());
	if (expected.metadata.has_value()) {
		fail_unless(got->metadata->dimension == expected.metadata->dimension);
		size_t dimension = expected.metadata->dimension;
		fail_unless(got->metadata->column_maps.size() == dimension);
		for (size_t i = 0; i < dimension; i++) {
			const ColumnMap &got_cm = got->metadata->column_maps[i];
			const ColumnMap &expected_cm = expected.metadata->column_maps[i];
			/* Values. */
			size_t got_len = got_cm.field_name_len;
			size_t expected_len = expected_cm.field_name_len;
			fail_unless(got_len == expected_len);
			fail_unless(strcmp(got_cm.field_name, expected_cm.field_name) == 0);
			/* Types. */
			size_t got_type_len = got_cm.field_type_len;
			size_t expected_type_len = expected_cm.field_type_len;
			fail_unless(got_type_len == expected_type_len);
			fail_unless(strcmp(got_cm.field_type, expected_cm.field_type) == 0);
			/* Collations. */
			size_t got_coll_len = got_cm.collation_len;
			size_t expected_coll_len = expected_cm.collation_len;
			fail_unless(got_coll_len == expected_coll_len);
			fail_unless(strcmp(got_cm.collation, expected_cm.collation) == 0);
			/* Span. */
			size_t got_span_len = got_cm.span_len;
			size_t expected_span_len = expected_cm.span_len;
			fail_unless(got_span_len == expected_span_len);
			fail_unless(strcmp(got_cm.span, expected_cm.span) == 0);
			/* Flags. */
			fail_unless(got_cm.is_nullable == expected_cm.is_nullable);
			fail_unless(got_cm.is_autoincrement == expected_cm.is_autoincrement);
		}
	}

	/* Statement id. */
	fail_unless(got->stmt_id == expected.stmt_id);

	/* Bind count. */
	fail_unless(got->bind_count == expected.bind_count);

	/* Sql info. */
	fail_unless(got->sql_info.has_value() == expected.sql_info.has_value());
	if (expected.sql_info.has_value()) {
		fail_unless(got->sql_info->row_count == expected.sql_info->row_count);
		fail_unless(got->sql_info->autoincrement_id_count ==
			    expected.sql_info->autoincrement_id_count);
		size_t autoinc_id_cnt = expected.sql_info->autoincrement_id_count;
		for (size_t i = 0; i < autoinc_id_cnt; i++) {
			fail_unless(got->sql_info->autoincrement_ids[i] ==
				    expected.sql_info->autoincrement_ids[i]);
		}
	}
}

/** Specialization of check_sql_data for the case when SqlData must be empty. */
void
check_sql_data(const std::optional<SqlData> &got, const std::optional<SqlData> &expected)
{
	(void)expected;
	fail_if(got.has_value());
}

/** Single connection, several executes. */
template <class BUFFER, class NetProvider, class StmtProcessor>
void
single_conn_sql(Connector<BUFFER, NetProvider> &client)
{
	TEST_INIT(0);

	Connection<Buf_t, NetProvider> conn(client);
	int rc = test_connect(client, conn, localhost, port);
	fail_unless(rc == 0);

	TEST_CASE("CREATE TABLE");
	std::string stmt_str = "CREATE TABLE IF NOT EXISTS tsql (column1 UNSIGNED PRIMARY KEY, "
			       "column2 VARCHAR(50), column3 DOUBLE);";
	auto stmt = StmtProcessor::process(client, conn, stmt_str);
	rid_t create_table = conn.execute(stmt, std::make_tuple());
	
	client.wait(conn, create_table, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(create_table));
	std::optional<Response<Buf_t>> response = conn.getResponse(create_table);
	fail_unless(response != std::nullopt);
	fail_unless(response->body.error_stack == std::nullopt);

	SqlData sql_data_create_table;
	sql_data_create_table.sql_info = SqlInfo{1};
	check_sql_data(response->body.data->sql_data, sql_data_create_table);

	TEST_CASE("Simple INSERT");
	stmt_str = "INSERT INTO tsql VALUES (20, 'first', 3.2), (21, 'second', 5.4)";
	stmt = StmtProcessor::process(client, conn, stmt_str);
	rid_t insert = conn.execute(stmt, std::make_tuple());
	
	client.wait(conn, insert, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(insert));
	response = conn.getResponse(insert);
	fail_unless(response != std::nullopt);
	fail_unless(response->body.error_stack == std::nullopt);

	/* Check metadata. */
	SqlData sql_data_insert;
	sql_data_insert.sql_info = SqlInfo{2};
	check_sql_data(response->body.data->sql_data, sql_data_insert);

	TEST_CASE("INSERT with binding arguments");
	std::tuple args = std::make_tuple(1, "Timur",   12.8,
	                                  2, "Nikita",  -8.0,
					  3, "Anastas", 345.298);
	stmt_str = "INSERT INTO tsql VALUES (?, ?, ?), (?, ?, ?), (?, ?, ?);";
	stmt = StmtProcessor::process(client, conn, stmt_str);
	rid_t insert_args = conn.execute(stmt, args);
	
	client.wait(conn, insert_args, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(insert_args));
	response = conn.getResponse(insert_args);
	fail_unless(response != std::nullopt);

	printResponse<BUFFER, NetProvider>(conn, *response);
	SqlData sql_data_insert_bind;
	sql_data_insert_bind.sql_info = SqlInfo{3};
	check_sql_data(response->body.data->sql_data, sql_data_insert_bind);

	TEST_CASE("SELECT");
	stmt_str = "SELECT * FROM SEQSCAN tsql;";
	stmt = StmtProcessor::process(client, conn, stmt_str);
	rid_t select = conn.execute(stmt, std::make_tuple());
	
	client.wait(conn, select, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(select));
	response = conn.getResponse(select);
	fail_unless(response != std::nullopt);
	fail_if(response->body.error_stack != std::nullopt);
	fail_unless(response->body.data != std::nullopt);
	fail_unless(response->body.data->dimension == 5);
	printResponse<BUFFER, NetProvider>(conn, *response);
	SqlData sql_data_select;
	std::vector<ColumnMap> sql_data_select_columns = {
		{"COLUMN1", 7, "unsigned", 8, "", 0, false, false, "", 0},
		{"COLUMN2", 7, "string", 6, "", 0, false, false, "", 0},
		{"COLUMN3", 7, "double", 6, "", 0, false, false, "", 0},
	};
	sql_data_select.metadata = Metadata{3, sql_data_select_columns};
	check_sql_data(response->body.data->sql_data, sql_data_select);

	TEST_CASE("DROP TABLE");
	stmt_str = "DROP TABLE IF EXISTS tsql;";
	stmt = StmtProcessor::process(client, conn, stmt_str);
	rid_t drop_table = conn.execute(stmt, std::make_tuple());
	
	client.wait(conn, drop_table, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(drop_table));
	response = conn.getResponse(drop_table);
	fail_unless(response != std::nullopt);
	fail_unless(response->body.data->sql_data->sql_info != std::nullopt);
	fail_if(response->body.error_stack != std::nullopt);

	TEST_CASE("CREATE TABLE with autoincrement");
	stmt_str = "CREATE TABLE IF NOT EXISTS tsql "
		   "(column1 UNSIGNED PRIMARY KEY AUTOINCREMENT, "
		   "column2 VARCHAR(50), column3 DOUBLE);";
	stmt = StmtProcessor::process(client, conn, stmt_str);
	create_table = conn.execute(stmt, std::make_tuple());
	client.wait(conn, create_table, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(create_table));
	response = conn.getResponse(create_table);
	fail_unless(response != std::nullopt);
	printResponse<BUFFER, NetProvider>(conn, *response);
	fail_unless(response->body.error_stack == std::nullopt);

	SqlData sql_data_create_table_autoinc;
	sql_data_create_table_autoinc.sql_info = SqlInfo{1};
	check_sql_data(response->body.data->sql_data, sql_data_create_table_autoinc);

	TEST_CASE("INSERT with autoincrement");
	std::tuple args2 = std::make_tuple(
		nullptr, "Timur", 12.8,
	        nullptr, "Nikita", -8.0,
		/* Null for the 1st field is in statement. */
		"Anastas", 345.298);
	stmt_str = "INSERT INTO tsql VALUES (?, ?, ?), (?, ?, ?), (NULL, ?, ?);";
	stmt = StmtProcessor::process(client, conn, stmt_str);
	insert = conn.execute(stmt, args2);
	client.wait(conn, insert, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(insert));
	response = conn.getResponse(insert);
	fail_unless(response != std::nullopt);
	printResponse<BUFFER, NetProvider>(conn, *response);
	fail_unless(response->body.error_stack == std::nullopt);

	SqlData sql_data_insert_autoinc;
	sql_data_insert_autoinc.sql_info = SqlInfo{3, 3, {1, 2, 3}};
	check_sql_data(response->body.data->sql_data, sql_data_insert_autoinc);

	TEST_CASE("SELECT from space with autoinc");
	stmt_str = "SELECT * FROM SEQSCAN tsql;";
	stmt = StmtProcessor::process(client, conn, stmt_str);
	select = conn.execute(stmt, std::make_tuple());
	
	client.wait(conn, select, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(select));
	response = conn.getResponse(select);
	fail_unless(response != std::nullopt);
	fail_if(response->body.error_stack != std::nullopt);
	fail_unless(response->body.data != std::nullopt);
	fail_unless(response->body.data->dimension == 3);
	printResponse<BUFFER, NetProvider>(conn, *response);
	SqlData sql_data_select_autoinc;
	sql_data_select_autoinc.metadata = Metadata{3, sql_data_select_columns};
	check_sql_data(response->body.data->sql_data, sql_data_select_autoinc);

	/* Finally, drop the table. */
	stmt_str = "DROP TABLE IF EXISTS tsql;";
	stmt = StmtProcessor::process(client, conn, stmt_str);
	drop_table = conn.execute(stmt, std::make_tuple());
	client.wait(conn, drop_table, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(drop_table));
	response = conn.getResponse(drop_table);
	fail_unless(response != std::nullopt);
	fail_unless(response->body.data->sql_data->sql_info != std::nullopt);
	fail_unless(response->body.error_stack == std::nullopt);

	/* TODO: test collations, span, is_nullbale, is_autoincrement. */

	client.close(conn);
}



/** Single connection, call procedure with arguments */
template <class BUFFER, class NetProvider>
void
replace_unix_socket(Connector<BUFFER, NetProvider> &client)
{
	TEST_INIT(0);

	Connection<Buf_t, NetProvider> conn(client);
	int rc = test_connect(client, conn, unixsocket, 0);
	fail_unless(rc == 0);

	TEST_CASE("select from unix socket");

	auto s = conn.space[512];

	rid_t f = s.replace(std::make_tuple(666, "111", 1.01));
	client.wait(conn, f, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(f));
	std::optional<Response<Buf_t>> response = conn.getResponse(f);
	fail_unless(response != std::nullopt);
	fail_unless(response->body.data != std::nullopt);
	fail_unless(response->body.error_stack == std::nullopt);
	printResponse<BUFFER, NetProvider>(conn, *response);

	client.close(conn);
}

/** Single connection, call procedure with arguments */
template <class BUFFER, class NetProvider>
void
test_auth(Connector<BUFFER, NetProvider> &client)
{
	TEST_INIT(0);
	const char *user = "megauser";
	const char *passwd  = "megapassword";

	Connection<Buf_t, NetProvider> conn(client);
	int rc = test_connect(client, conn, localhost, port, user, passwd);
	fail_unless(rc == 0);

	uint32_t space_id = 513;

	auto s = conn.space[space_id];
	rid_t f = s.select(std::make_tuple(0));
	client.wait(conn, f, WAIT_TIMEOUT);
	fail_unless(conn.futureIsReady(f));

	std::optional<Response<Buf_t>> response = conn.getResponse(f);
	fail_unless(response != std::nullopt);
	fail_unless(response->body.data != std::nullopt);
	fail_unless(response->body.error_stack == std::nullopt);
	printResponse<BUFFER, NetProvider>(conn, *response);
}

/** Single connection, write to closed connection. */
template <class BUFFER, class NetProvider>
void
test_sigpipe(Connector<BUFFER, NetProvider> &client)
{
	TEST_INIT(0);

	int rc = ::launchDummyServer(localhost, dummy_server_port);
	fail_unless(rc == 0);

	Connection<Buf_t, NetProvider> conn(client);
	rc = ::test_connect(client, conn, localhost, dummy_server_port);
	fail_unless(rc == 0);

	/*
	 * Create a large payload so that request needs at least 2 `send`s, the
	 * latter being written to a closed socket.
	 */
	rid_t f = conn.space[0].replace(std::vector<uint64_t>(100000, 777));
	fail_if(client.wait(conn, f, WAIT_TIMEOUT) == 0);
	fail_unless(conn.getError().saved_errno == EPIPE);
	fail_if(conn.futureIsReady(f));
}

/** Single connection, wait response from closed connection. */
template <class BUFFER, class NetProvider>
void
test_dead_connection_wait(Connector<BUFFER, NetProvider> &client)
{
	TEST_INIT(0);

	int rc = ::launchDummyServer(localhost, dummy_server_port);
	fail_unless(rc == 0);

	Connection<Buf_t, NetProvider> conn(client);
	rc = ::test_connect(client, conn, localhost, dummy_server_port);
	fail_unless(rc == 0);

	rid_t f = conn.ping();
	fail_if(client.wait(conn, f) == 0);
	fail_if(conn.futureIsReady(f));

	fail_if(client.waitAll(conn, std::vector<rid_t>(f)) == 0);
	fail_if(conn.futureIsReady(f));

	fail_if(client.waitCount(conn, 1) == 0);
	fail_if(conn.futureIsReady(f));

	/* FIXME(gh-51) */
#if 0
	fail_if(client.waitAny() != std::nullopt);
	fail_if(conn.futureIsReady(f));
#endif
}
int main()
{
	if (cleanDir() != 0)
		return -1;

#ifdef TNTCXX_ENABLE_SSL
	if (genSSLCert() != 0)
		return -1;
#endif

	if (launchTarantool(enable_ssl) != 0)
		return -1;

	sleep(1);

	Connector<Buf_t, NetProvider> client;
	trivial<Buf_t, NetProvider>(client);
	single_conn_ping<Buf_t, NetProvider>(client);
	auto_close<Buf_t, NetProvider>(client);
	many_conn_ping<Buf_t, NetProvider>(client);
	single_conn_error<Buf_t, NetProvider>(client);
	single_conn_replace<Buf_t, NetProvider>(client);
	single_conn_insert<Buf_t, NetProvider>(client);
	single_conn_update<Buf_t, NetProvider>(client);
	single_conn_delete<Buf_t, NetProvider>(client);
	single_conn_upsert<Buf_t, NetProvider>(client);
	single_conn_select<Buf_t, NetProvider>(client);
	single_conn_call<Buf_t, NetProvider>(client);
	single_conn_sql<Buf_t, NetProvider, Stmt>(client);
	single_conn_sql<Buf_t, NetProvider, StmtPrepare>(client);
	replace_unix_socket(client);
	test_auth(client);
	/*
	 * Testing this for SSL is hard, since the connection starts to involve
	 * an a lot more complex state machine.
	 */
#ifndef TNTCXX_ENABLE_SSL
	// ::test_sigpipe(client);
#endif
	::test_dead_connection_wait(client);
	return 0;
}
