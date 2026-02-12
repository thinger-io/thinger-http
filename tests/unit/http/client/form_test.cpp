#include <catch2/catch_test_macros.hpp>
#include <thinger/http/client/form.hpp>

using namespace thinger::http;

TEST_CASE("Form URL Encoding", "[form][unit]") {

    SECTION("url_encode basic characters") {
        REQUIRE(form::url_encode("hello") == "hello");
        REQUIRE(form::url_encode("Hello World") == "Hello+World");
        REQUIRE(form::url_encode("foo@bar.com") == "foo%40bar.com");
        REQUIRE(form::url_encode("100%") == "100%25");
        REQUIRE(form::url_encode("a=b&c=d") == "a%3Db%26c%3Dd");
    }

    SECTION("url_encode special characters") {
        REQUIRE(form::url_encode("José") == "Jos%C3%A9");
        REQUIRE(form::url_encode("日本語") == "%E6%97%A5%E6%9C%AC%E8%AA%9E");
    }

    SECTION("url_decode reverses url_encode") {
        REQUIRE(form::url_decode("Hello+World") == "Hello World");
        REQUIRE(form::url_decode("foo%40bar.com") == "foo@bar.com");
        REQUIRE(form::url_decode("100%25") == "100%");
    }
}

TEST_CASE("Form Fields", "[form][unit]") {

    SECTION("single field") {
        form f;
        f.field("name", "John");

        REQUIRE_FALSE(f.empty());
        REQUIRE_FALSE(f.is_multipart());
        REQUIRE(f.content_type() == "application/x-www-form-urlencoded");
        REQUIRE(f.body() == "name=John");
    }

    SECTION("multiple fields") {
        form f;
        f.field("username", "john")
         .field("password", "secret123");

        REQUIRE(f.body() == "username=john&password=secret123");
    }

    SECTION("fields with special characters") {
        form f;
        f.field("email", "user@example.com")
         .field("message", "Hello World!");

        REQUIRE(f.body() == "email=user%40example.com&message=Hello+World%21");
    }

    SECTION("initializer list fields") {
        form f;
        f.fields({
            {"a", "1"},
            {"b", "2"},
            {"c", "3"}
        });

        REQUIRE(f.body() == "a=1&b=2&c=3");
    }
}

TEST_CASE("Form Files", "[form][unit]") {

    SECTION("file from memory makes multipart") {
        form f;
        f.field("name", "test")
         .file("data", "file content here", "test.txt", "text/plain");

        REQUIRE(f.is_multipart());
        REQUIRE(f.content_type().find("multipart/form-data") != std::string::npos);
        REQUIRE(f.content_type().find("boundary=") != std::string::npos);
    }

    SECTION("multipart body format") {
        form f;
        f.field("name", "John")
         .file("doc", "Hello World", "hello.txt", "text/plain");

        std::string body = f.body();

        // Should contain field
        REQUIRE(body.find("Content-Disposition: form-data; name=\"name\"") != std::string::npos);
        REQUIRE(body.find("John") != std::string::npos);

        // Should contain file
        REQUIRE(body.find("Content-Disposition: form-data; name=\"doc\"; filename=\"hello.txt\"") != std::string::npos);
        REQUIRE(body.find("Content-Type: text/plain") != std::string::npos);
        REQUIRE(body.find("Hello World") != std::string::npos);

        // Should have closing boundary
        REQUIRE(body.find("--") != std::string::npos);
    }

    SECTION("file from binary data") {
        std::vector<uint8_t> data = {0x89, 0x50, 0x4E, 0x47};  // PNG header
        form f;
        f.file("image", data.data(), data.size(), "test.png", "image/png");

        REQUIRE(f.is_multipart());
        std::string body = f.body();
        REQUIRE(body.find("Content-Type: image/png") != std::string::npos);
    }
}

TEST_CASE("Form MIME Type Detection", "[form][unit]") {

    SECTION("common image types") {
        REQUIRE(form::mime_type("photo.jpg") == "image/jpeg");
        REQUIRE(form::mime_type("photo.jpeg") == "image/jpeg");
        REQUIRE(form::mime_type("image.png") == "image/png");
        REQUIRE(form::mime_type("animation.gif") == "image/gif");
        REQUIRE(form::mime_type("icon.svg") == "image/svg+xml");
    }

    SECTION("document types") {
        REQUIRE(form::mime_type("doc.pdf") == "application/pdf");
        REQUIRE(form::mime_type("data.json") == "application/json");
        REQUIRE(form::mime_type("page.html") == "text/html");
        REQUIRE(form::mime_type("styles.css") == "text/css");
    }

    SECTION("archive types") {
        REQUIRE(form::mime_type("archive.zip") == "application/zip");
        REQUIRE(form::mime_type("archive.gz") == "application/gzip");
    }

    SECTION("unknown type defaults to octet-stream") {
        REQUIRE(form::mime_type("file.xyz") == "application/octet-stream");
    }

    SECTION("file without extension returns text/plain") {
        // Files without extension get text/plain (library default)
        REQUIRE(form::mime_type("noext") == "text/plain");
    }

    SECTION("case insensitive") {
        REQUIRE(form::mime_type("PHOTO.JPG") == "image/jpeg");
        REQUIRE(form::mime_type("Doc.PDF") == "application/pdf");
    }
}

TEST_CASE("Form Empty State", "[form][unit]") {

    SECTION("new form is empty") {
        form f;
        REQUIRE(f.empty());
        REQUIRE_FALSE(f.is_multipart());
    }

    SECTION("form with field is not empty") {
        form f;
        f.field("x", "y");
        REQUIRE_FALSE(f.empty());
    }

    SECTION("form with file is not empty") {
        form f;
        f.file("f", std::string("content"), "file.txt");
        REQUIRE_FALSE(f.empty());
    }
}
