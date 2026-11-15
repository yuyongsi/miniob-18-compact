"""Check all 18 basic-training problems against a real MiniOB server."""
import argparse
from contextlib import contextmanager
from pathlib import Path
import socket
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]


@contextmanager
def server(observer, *args):
    with tempfile.TemporaryDirectory(prefix="miniob-basic-") as directory:
        address = Path(directory) / "observer.sock"
        with (Path(directory) / "server.log").open("w+") as log:
            process = subprocess.Popen(
                [str(observer), "-f", str(ROOT / "etc/observer.ini"), "-s", str(address), *args],
                cwd=directory, stdout=log, stderr=log,
            )
            try:
                with socket.socket(socket.AF_UNIX) as client:
                    client.settimeout(10)
                    deadline = time.monotonic() + 12
                    while True:
                        try:
                            client.connect(str(address))
                            break
                        except (FileNotFoundError, ConnectionRefusedError):
                            if process.poll() is not None or time.monotonic() > deadline:
                                log.seek(0)
                                raise RuntimeError(log.read()[-4000:])
                            time.sleep(0.05)

                    def sql(statement):
                        client.sendall(statement.encode() + b"\0")
                        response = bytearray()
                        while not response.endswith(b"\0"):
                            chunk = client.recv(65536)
                            if not chunk:
                                raise RuntimeError(f"Server closed the connection on {statement[:120]}")
                            response.extend(chunk)
                        return response[:-1].decode().strip()

                    yield sql
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=8)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--observer", type=Path, default=ROOT / "build/bin/observer")
    args = parser.parse_args()
    results = {}
    failures = []

    def run_cases(sql, topic, cases):
        counts = results.setdefault(topic, [0, 0])
        for statement, expected in cases:
            actual = sql(statement)
            counts[1] += 1
            if actual.casefold() == expected.casefold():
                counts[0] += 1
            else:
                failures.append(f"{topic}: {statement[:125]}\n  expected {expected[:180]!r}\n  got      {actual[:180]!r}")

    with server(args.observer.resolve()) as sql:
        def check(topic, *cases):
            run_cases(sql, topic, cases)

        check("basic",
              ("create table b(id int, age int, name char(8), score float);", "SUCCESS"),
              ("insert into b values(1,18,'ann',1.5);", "SUCCESS"),
              ("insert into b values(2,20,'bob',2.25);", "SUCCESS"),
              ("create index b_idx on b(id);", "SUCCESS"),
              ("select * from b where id=2;", "id | age | name | score\n2 | 20 | bob | 2.25"),
              ("delete from b where id=2;", "SUCCESS"),
              ("select id from b where id=2;", "id"))

        check("group-by",
              ("create table gp(dept int, wage int);", "SUCCESS"),
              ("insert into gp values(1,10);", "SUCCESS"),
              ("insert into gp values(1,30);", "SUCCESS"),
              ("insert into gp values(2,40);", "SUCCESS"),
              ("select dept, count(*), avg(wage) from gp group by dept order by dept;",
               "dept | count(*) | avg(wage)\n1 | 2 | 20\n2 | 1 | 40"),
              ("select dept, max(wage) from gp group by dept having count(*) > 1;",
               "dept | max(wage)\n1 | 30"))

        check("order-by",
              ("create table ord(id int, score int);", "SUCCESS"),
              ("insert into ord values(2,30);", "SUCCESS"),
              ("insert into ord values(3,10);", "SUCCESS"),
              ("insert into ord values(1,30);", "SUCCESS"),
              ("select id,score from ord order by score desc,id;", "id | score\n1 | 30\n2 | 30\n3 | 10"),
              ("select id from ord order by id;", "id\n1\n2\n3"))

        check("complex-sub-query",
              ("create table parent(id int, score int);", "SUCCESS"),
              ("create table child(id int, score int);", "SUCCESS"),
              ("create table tags(id int);", "SUCCESS"),
              ("insert into parent values(1,30);", "SUCCESS"),
              ("insert into parent values(2,5);", "SUCCESS"),
              ("insert into child values(1,15);", "SUCCESS"),
              ("insert into child values(2,10);", "SUCCESS"),
              ("insert into tags values(1);", "SUCCESS"),
              ("select p.id from parent p where p.score > "
               "(select max(c.score) from child c,tags t where c.id=p.id and c.id=t.id);", "id\n1"),
              ("select id from parent where id in (select id from child where id in (select id from tags));",
               "id\n1"))

        check("expression",
              ("select (b.age+2)*3-1 from b;", "(b.age+2)*3-1\n59"),
              ("select b.age/0 from b;", "b.age/0\nNULL"),
              ("select id from b where (age+2)*3>=60;", "id\n1"))

        text_limit = 4096
        overflow = "x" * text_limit + "discard"
        text_prefix = "xy" * (text_limit // 2)
        check("text",
              ("create table messages(id int, body text);", "SUCCESS"),
              ("insert into messages values(1,'short text');", "SUCCESS"),
              (f"insert into messages values(2,'{overflow}');", "SUCCESS"),
              ("select id,length(body) from messages order by id;", "id | length(body)\n1 | 10\n2 | 4096"),
              ("select body from messages where id=2;", "body\n" + "x" * text_limit),
              (f"update messages set body='{text_prefix}beyond' where id=1;", "SUCCESS"),
              ("select body from messages where id=1;", "body\n" + text_prefix),
              ("delete from messages where id=2;", "SUCCESS"),
              ("select count(*) from messages;", "count(*)\n1"))

        check("multi-index",
              ("create table mi(a int, b int, tag char(8));", "SUCCESS"),
              ("insert into mi values(1,3,'a');", "SUCCESS"),
              ("insert into mi values(2,3,'b');", "SUCCESS"),
              ("insert into mi values(2,4,'c');", "SUCCESS"),
              ("create index mi_ab on mi(a,b);", "SUCCESS"),
              ("select tag from mi where a=2 order by b;", "tag\nb\nc"),
              ("select tag from mi where a=2 and b=4;", "tag\nc"),
              ("select tag from mi where b=3 order by a;", "tag\na\nb"),
              ("update mi set a=3 where tag='c';", "SUCCESS"),
              ("select tag from mi where a=3 and b=4;", "tag\nc"))

        check("simple-sub-query",
              ("select id from parent where id in (select id from tags);", "id\n1"),
              ("select id from parent where id not in (select id from tags);", "id\n2"),
              ("select id from parent where score>(select max(score) from child);", "id\n1"),
              ("select id from parent where score=(select score from child);", "FAILURE"))

        check("null",
              ("create table nl(id int, n int nullable, m int null);", "SUCCESS"),
              ("insert into nl values(1,null,null);", "SUCCESS"),
              ("insert into nl values(2,10,20);", "SUCCESS"),
              ("insert into nl values(null,1,2);", "FAILURE"),
              ("select id from nl where n is null;", "id\n1"),
              ("select id from nl where n=null;", "id"),
              ("select id,n+1 from nl order by id;", "id | n+1\n1 | NULL\n2 | 11"),
              ("select count(*),count(n),avg(n) from nl;", "count(*) | count(n) | avg(n)\n2 | 1 | 10"),
              ("select id from nl where id not in (select n from nl);", "id"),
              ("create table nl_legacy(id int, address nullable);", "SUCCESS"),
              ("insert into nl_legacy values(1,null);", "SUCCESS"),
              ("update nl_legacy set id=null;", "FAILURE"),
              ("select * from nl_legacy;", "id | address\n1 | NULL"))

        check("unique",
              ("create table uq(id int, n int);", "SUCCESS"),
              ("create unique index uq_id on uq(id);", "SUCCESS"),
              ("insert into uq values(1,10);", "SUCCESS"),
              ("insert into uq values(1,11);", "FAILURE"),
              ("insert into uq values(2,20);", "SUCCESS"),
              ("update uq set id=1 where id=2;", "FAILURE"),
              ("select * from uq order by id;", "id | n\n1 | 10\n2 | 20"))

        check("insert",
              ("create table batch(id int, n int);", "SUCCESS"),
              ("create unique index batch_id on batch(id);", "SUCCESS"),
              ("create index batch_n on batch(n);", "SUCCESS"),
              ("insert into batch values(1,10),(2,20),(3,30);", "SUCCESS"),
              ("select * from batch order by id;", "id | n\n1 | 10\n2 | 20\n3 | 30"),
              ("insert into batch values(4,40),(2,200),(5,50);", "FAILURE"),
              ("select id from batch where n=40;", "id"),
              ("insert into batch values(4,40),(4,41);", "FAILURE"),
              ("insert into batch values(4,40),(5);", "FAILURE"),
              ("insert into batch values(4,40),(5,'invalid');", "FAILURE"),
              ("insert into batch values(4,40),(5,null);", "FAILURE"),
              ("select * from batch order by id;", "id | n\n1 | 10\n2 | 20\n3 | 30"),
              ("insert into batch(n,id) values(40,4),(50,5);", "SUCCESS"),
              ("select * from batch order by id;", "id | n\n1 | 10\n2 | 20\n3 | 30\n4 | 40\n5 | 50"))

        check("aggregation-func",
              ("select max(age),min(age),count(*),count(1),avg(age) from b;",
               "max(age) | min(age) | count(*) | count(1) | avg(age)\n18 | 18 | 1 | 1 | 18"),
              ("select min(name),max(name) from b;", "min(name) | max(name)\nann | ann"),
              ("select count(*),max(age) from b where age>100;", "count(*) | max(age)\n0 | NULL"),
              ("select count() from b;", "FAILURE"))

        check("select-tables",
              ("select * from tags, b;", "tags.id | b.id | b.age | b.name | b.score\n1 | 1 | 18 | ann | 1.5"),
              ("select b.*,tags.id from tags,b;", "b.id | b.age | b.name | b.score | tags.id\n1 | 18 | ann | 1.5 | 1"),
              ("select b.id from b,tags where b.id<>tags.id;", "b.id"),
              ("select b.id from b,tags where b.id!=tags.id;", "b.id"))

        check("date",
              ("create table birthdays(id int, d date);", "SUCCESS"),
              ("create index birth_date on birthdays(d);", "SUCCESS"),
              ("insert into birthdays values(1,'2000-2-29');", "SUCCESS"),
              ("insert into birthdays values(2,'2001-2-29');", "FAILURE"),
              ("insert into birthdays values(3,'2024-2-30');", "FAILURE"),
              ("select * from birthdays where d='2000-2-29';", "id | d\n1 | 2000-02-29"),
              ("select id from birthdays where d<'2001-1-1';", "id\n1"),
              ("select id from birthdays where d='2001-02-29';", "FAILURE"))

        check("update",
              ("create table ut(id int, n int);", "SUCCESS"),
              ("insert into ut values(1,20);", "SUCCESS"),
              ("insert into ut values(2,30);", "SUCCESS"),
              ("create index ut_id on ut(id);", "SUCCESS"),
              ("update ut set id=3 where id=2;", "SUCCESS"),
              ("select id from ut where id=2;", "id"),
              ("select id from ut where id=3;", "id\n3"),
              ("update ut set n=42;", "SUCCESS"),
              ("update ut set n=1 where id=99;", "SUCCESS"),
              ("update ut set n='invalid';", "FAILURE"),
              ("update ut set missing=1;", "FAILURE"),
              ("select * from ut order by id;", "id | n\n1 | 42\n3 | 42"))

        check("join-tables",
              ("select parent.id, child.score from parent inner join child "
               "on parent.id=child.id and child.score>10 inner join tags on tags.id=parent.id;",
               "parent.id | child.score\n1 | 15"),
              ("select parent.id from parent inner join child on parent.id=child.id where parent.score>child.score;",
               "parent.id\n1"))

        check("drop-table",
              ("create table gone(id int);", "SUCCESS"),
              ("create index gone_idx on gone(id);", "SUCCESS"),
              ("insert into gone values(1);", "SUCCESS"),
              ("drop table gone;", "SUCCESS"),
              ("select * from gone;", "FAILURE"),
              ("drop table gone;", "FAILURE"),
              ("create table gone(id int);", "SUCCESS"),
              ("create index gone_idx on gone(id);", "SUCCESS"),
              ("select * from gone;", "id"))

        check("select-meta",
              ("select * from no_table;", "FAILURE"),
              ("select missing from b;", "FAILURE"),
              ("select * from b where missing=1;", "FAILURE"),
              ("select absent.id from b;", "FAILURE"),
              ("select id from b,tags;", "FAILURE"),
              ("select *,id from b where age=18;", "id | age | name | score | id\n1 | 18 | ann | 1.5 | 1"))

    with server(args.observer.resolve(), "-t", "mvcc") as sql:
        run_cases(sql, "insert", (
            ("create table mt(id int);", "SUCCESS"),
            ("create unique index mt_idx on mt(id);", "SUCCESS"),
            ("begin;", "SUCCESS"),
            ("insert into mt values(1);", "SUCCESS"),
            ("insert into mt values(2),(1);", "FAILURE"),
            ("select id from mt order by id;", "id\n1"),
            ("insert into mt values(2),(3);", "SUCCESS"),
            ("commit;", "SUCCESS"),
            ("select id from mt order by id;", "id\n1\n2\n3"),
            ("begin;", "SUCCESS"),
            ("insert into mt values(4),(5);", "SUCCESS"),
            ("rollback;", "SUCCESS"),
            ("select id from mt order by id;", "id\n1\n2\n3"),
        ))

    for topic, (passed, total) in results.items():
        print(f"{topic}: {passed}/{total}")
    print(f"{sum(p == t for p, t in results.values())}/{len(results)} topics, "
          f"{sum(p for p, _ in results.values())}/{sum(t for _, t in results.values())} checks")
    for failure in failures:
        print(failure)
    return int(bool(failures))


if __name__ == "__main__":
    raise SystemExit(main())
