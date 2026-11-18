-- Phase 1: INSERT 와 SELECT 가 파일 기반으로 동작한다
-- id 는 자동으로 1 부터 붙는다
INSERT INTO users VALUES ('kim', 'kim@jungle.kr', 20);
INSERT INTO users VALUES ('lee', 'lee@jungle.kr', 31);
INSERT INTO users (age, name) VALUES (27, 'park');  -- 순서를 바꾼 컬럼 목록, email 은 빈 값

SELECT * FROM users;
SELECT name, age FROM users WHERE age = 20;
SELECT id, name FROM users WHERE name = 'park';
select id from users where id between 2 and 3;
