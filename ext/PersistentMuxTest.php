<?php
require 'PHPUnit/Framework/ExtensionTestCase.php';
require 'PHPUnit/TestMore.php';
require '../src/Pux/PatternCompiler.php';
require '../src/Pux/MuxCompiler.php';
require '../src/Pux/Executor.php';
use Pux\Mux;
use Pux\Executor;

class PersistentMuxTest extends PHPUnit_Framework_TestCase
{

    public function testStore() {
        $mux = new Mux;
        ok( pux_store_mux('phpunit1', $mux), 'mux stored' );
    }
}
