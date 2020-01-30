const path = require('path');
const { Application } = require('spectron');

function initialiseSpectron() {
    let electronPath = path.join(__dirname, "../node_modules", ".bin", "electron");
    const appPath = path.join(__dirname, "../index.js");    
    if (process.platform === "win32") {
        electronPath += ".cmd";
    }
 
    return new Application({
        path: electronPath,
        args: [appPath],
        env: {
            ELECTRON_ENABLE_LOGGING: true,
            ELECTRON_ENABLE_STACK_DUMPING: true,
            NODE_ENV: "development"
        },
        startTimeout: 20000,
        chromeDriverLogPath: 'chromedriverlog.txt'
   });
 }

const app = initialiseSpectron();

const chaiAsPromised = require("chai-as-promised");
const chai = require("chai");
chai.should();
chai.use(chaiAsPromised);

describe("blazor-test-app", function () {
    this.timeout(20000);
    // CSS selectors
    const counterNavBtn = 'body > app > div.sidebar > div.collapse > ul > li:nth-child(2) > a';
    const currentCounterDisplay = 'body > app > div.main > div.content.px-4 > p:nth-child(3)';
    const counterIncrementButton = "body > app > div.main > div.content.px-4 > button";
    const counterInitializedDisplay = 'body > app > div.main > div.content.px-4 > p:nth-child(7)';
   

    // Start spectron
    before(function () {
        chaiAsPromised.transferPromiseness = app.transferPromiseness;
        return app.start();
    });

    // Stop Electron
    after(function () {
        if (app && app.isRunning()) {
            return app.stop();
        }
    });

    describe("Home", function () {
        // wait for Electron window to open
        it('open window', function () {
            return app.client.waitUntilWindowLoaded().getWindowCount().should.eventually.equal(1);
        });

        // click on link in sidebar
        it("go to counter", async function () {
            await app.client.element(counterNavBtn).click();
            await app.client.getText(currentCounterDisplay).should.eventually.equal('Current count: 0')
            await app.client.getText(counterInitializedDisplay).should.eventually.equal("OnInitializedAsync")
            await app.client.element(counterIncrementButton).isEnabled().should.eventually.equal(false);
            await app.client.waitUntilTextExists(counterInitializedDisplay, "OnInitializedAsync after delay");

            
            return await app.client
                .getText(currentCounterDisplay).should.eventually.equal('Current count: 5')
                .element(counterIncrementButton).isEnabled().should.eventually.equal(true);                
        });

        // The password required validator should be activated when the textfield is empty
        it("increment counter", function () {
            return app.client
                .click(counterIncrementButton)
                .waitUntilTextExists(currentCounterDisplay, 'Current count: 6');
        });
    });
});